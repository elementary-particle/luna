#include "blend2d/canvas.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace luna::backend::blend2d {

namespace {

struct ShapedText {
  BLPath path;
  TextMetrics metrics;
};

bool IsHorizontalWhitespace(unsigned char ch) {
  switch (ch) {
  case ' ':
  case '\t':
  case '\v':
  case '\f':
    return true;
  default:
    return false;
  }
}

size_t Utf8CodepointLength(std::string_view text, size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if ((lead & 0x80u) == 0) {
    return 1;
  }
  if ((lead & 0xE0u) == 0xC0u) {
    return std::min<size_t>(2, text.size() - offset);
  }
  if ((lead & 0xF0u) == 0xE0u) {
    return std::min<size_t>(3, text.size() - offset);
  }
  if ((lead & 0xF8u) == 0xF0u) {
    return std::min<size_t>(4, text.size() - offset);
  }
  return 1;
}

size_t PrevUtf8Boundary(std::string_view text, size_t end) {
  if (end == 0) {
    return 0;
  }

  size_t index = end - 1;
  while (index > 0 &&
      (static_cast<unsigned char>(text[index]) & 0xC0u) == 0x80u) {
    --index;
  }
  return index;
}

void SetError(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

void PopulateTextMetrics(
    const BLFont &font, const BLTextMetrics &bl_metrics, TextMetrics *metrics) {
  const BLFontMetrics &font_metrics = font.metrics();
  metrics->advance_width = bl_metrics.advance.x;
  metrics->bounds_x = bl_metrics.bounding_box.x0;
  metrics->bounds_y = bl_metrics.bounding_box.y0;
  metrics->bounds_w = bl_metrics.bounding_box.x1 - bl_metrics.bounding_box.x0;
  metrics->bounds_h = bl_metrics.bounding_box.y1 - bl_metrics.bounding_box.y0;
  metrics->ascent = -static_cast<double>(font_metrics.ascent);
  metrics->descent = static_cast<double>(font_metrics.descent);
  metrics->leading = static_cast<double>(font_metrics.line_gap);
  metrics->line_height =
      metrics->descent - metrics->ascent + metrics->leading;
}

bool ResolveFont(const BLFontManager &font_mgr, const Font &font,
    const char *op, BLFont *resolved_font, std::string *error) {
  if (!(font.size > 0.0)) {
    SetError(error, std::string(op) + ": font.size must be positive");
    return false;
  }
  if (!font_mgr.is_valid()) {
    SetError(error, std::string(op) + ": canvas has no font manager");
    return false;
  }
  if (font.family.empty()) {
    SetError(error,
        std::string(op) + ": Blend2D backend requires font.family");
    return false;
  }

  BLFontQueryProperties query{};
  query.style = font.slant;
  query.weight = font.weight;
  query.stretch = font.width;

  BLFontFace face;
  if (font_mgr.query_face(font.family.c_str(), query, face) != BL_SUCCESS ||
      !face.is_valid()) {
    SetError(error,
        std::string(op) + ": font family '" + font.family + "' not found");
    return false;
  }

  resolved_font->reset();
  if (resolved_font->create_from_face(face, static_cast<float>(font.size)) !=
          BL_SUCCESS ||
      !resolved_font->is_valid()) {
    SetError(error,
        std::string(op) + ": failed to create font '" + font.family + "'");
    return false;
  }

  return true;
}

bool ShapeText(const BLFontManager &font_mgr, const Font &font,
    std::string_view text, const char *op, ShapedText *shaped,
    std::string *error) {
  BLFont resolved_font;
  if (!ResolveFont(font_mgr, font, op, &resolved_font, error)) {
    return false;
  }

  BLTextMetrics bl_metrics{};
  TextMetrics metrics{};
  PopulateTextMetrics(resolved_font, bl_metrics, &metrics);

  BLPath path;
  if (!text.empty()) {
    BLGlyphBuffer glyph_buffer;
    if (glyph_buffer.set_utf8_text(text.data(), text.size()) != BL_SUCCESS) {
      SetError(error, std::string(op) + ": failed to decode UTF-8 text");
      return false;
    }
    if (resolved_font.shape(glyph_buffer) != BL_SUCCESS) {
      SetError(error, std::string(op) + ": Blend2D shaping failed");
      return false;
    }

    const BLGlyphRun &glyph_run = glyph_buffer.glyph_run();
    if (glyph_run.placement_type != BL_GLYPH_PLACEMENT_TYPE_NONE &&
        glyph_run.placement_type != BL_GLYPH_PLACEMENT_TYPE_ADVANCE_OFFSET) {
      SetError(error,
          std::string(op) +
              ": unsupported glyph placement for simple text layout");
      return false;
    }

    if (resolved_font.get_text_metrics(glyph_buffer, bl_metrics) !=
        BL_SUCCESS) {
      SetError(error, std::string(op) + ": failed to measure glyph run");
      return false;
    }
    PopulateTextMetrics(resolved_font, bl_metrics, &metrics);

    if (!glyph_run.is_empty() &&
        resolved_font.get_glyph_run_outlines(glyph_run, path) != BL_SUCCESS) {
      SetError(error, std::string(op) + ": failed to build glyph outlines");
      return false;
    }
  }

  shaped->path = std::move(path);
  shaped->metrics = metrics;
  return true;
}

void UpdateLineMetrics(Paragraph::Line *line, const ShapedText &shaped) {
  line->ascent = std::max(line->ascent, -shaped.metrics.ascent);
  line->descent = std::max(line->descent, shaped.metrics.descent);
  line->leading = std::max(line->leading, shaped.metrics.leading);
}

void RecomputeLineTextRange(Paragraph::Line *line) {
  bool have_text_range = false;
  size_t start_index = line->start_index;
  size_t end_index = line->end_index;

  for (const Paragraph::Run &run : line->runs) {
    if (run.synthetic) {
      continue;
    }
    if (!have_text_range) {
      start_index = run.start_index;
      have_text_range = true;
    }
    end_index = run.end_index;
  }

  if (have_text_range) {
    line->start_index = start_index;
    line->end_index = end_index;
  }
}

void AddRun(Paragraph::Line *line, const Paragraph::Style &style,
    std::string_view text, bool whitespace, size_t start_index,
    size_t end_index, bool synthetic, ShapedText &&shaped) {
  Paragraph::Run run;
  run.path = std::move(shaped.path);
  run.font = style.font;
  run.text.assign(text.data(), text.size());
  run.color = style.color;
  run.whitespace = whitespace;
  run.synthetic = synthetic;
  run.start_index = start_index;
  run.end_index = end_index;
  run.x = line->width;
  run.width = shaped.metrics.advance_width;
  run.ascent = -shaped.metrics.ascent;
  run.descent = shaped.metrics.descent;
  run.leading = shaped.metrics.leading;

  line->width += run.width;
  line->ascent = std::max(line->ascent, run.ascent);
  line->descent = std::max(line->descent, run.descent);
  line->leading = std::max(line->leading, run.leading);
  line->runs.push_back(std::move(run));
  RecomputeLineTextRange(line);
}

bool SeedEmptyLine(const BLFontManager &font_mgr, Paragraph::Line *line,
    const Paragraph::Style &style, std::string *error) {
  if (line->ascent > 0.0 || line->descent > 0.0 || line->leading > 0.0) {
    return true;
  }

  ShapedText shaped;
  if (!ShapeText(font_mgr, style.font, "", "paragraph", &shaped, error)) {
    return false;
  }
  UpdateLineMetrics(line, shaped);
  return true;
}

double AlignOffset(TextAlign align, double width, double line_width) {
  switch (align) {
  case TextAlign::kCenter:
    return std::max(0.0, (width - line_width) * 0.5);
  case TextAlign::kRight:
    return std::max(0.0, width - line_width);
  case TextAlign::kLeft:
  default:
    return 0.0;
  }
}

void RecomputeLine(Paragraph::Line *line, const Paragraph::Style &fallback) {
  line->width = 0.0;
  line->ascent = 0.0;
  line->descent = 0.0;
  line->leading = 0.0;

  for (Paragraph::Run &run : line->runs) {
    run.x = line->width;
    line->width += run.width;
    line->ascent = std::max(line->ascent, run.ascent);
    line->descent = std::max(line->descent, run.descent);
    line->leading = std::max(line->leading, run.leading);
  }

  if (line->runs.empty()) {
    line->ascent = std::max(line->ascent, fallback.font.size);
  }

  RecomputeLineTextRange(line);
}

struct PrefixFit {
  size_t bytes = 0;
  ShapedText shaped;
};

bool ShapeLargestPrefixThatFits(const BLFontManager &font_mgr,
    const Paragraph::Style &style, std::string_view text, double width,
    PrefixFit *fit, std::string *error) {
  PrefixFit best;
  bool have_best = false;
  size_t offset = 0;

  while (offset < text.size()) {
    offset += Utf8CodepointLength(text, offset);

    ShapedText shaped;
    if (!ShapeText(font_mgr, style.font, text.substr(0, offset), "paragraph",
            &shaped, error)) {
      return false;
    }

    if (shaped.metrics.advance_width <= width || !have_best) {
      best.bytes = offset;
      best.shaped = std::move(shaped);
      have_best = true;
    }

    if (have_best && best.shaped.metrics.advance_width > width) {
      break;
    }
    if (have_best && shaped.metrics.advance_width > width) {
      break;
    }
  }

  if (!have_best) {
    return false;
  }

  *fit = std::move(best);
  return true;
}

bool TrimRunToFitEllipsis(const BLFontManager &font_mgr, Paragraph::Run *run,
    double available_width, std::string *error) {
  size_t end = run->text.size();
  while (end > 0) {
    end = PrevUtf8Boundary(run->text, end);

    ShapedText shaped;
    if (!ShapeText(font_mgr, run->font,
            std::string_view(run->text.data(), end), "paragraph", &shaped,
            error)) {
      return false;
    }

    if (shaped.metrics.advance_width <= available_width || end == 0) {
      run->path = std::move(shaped.path);
      run->text.resize(end);
      run->width = shaped.metrics.advance_width;
      run->ascent = -shaped.metrics.ascent;
      run->descent = shaped.metrics.descent;
      run->leading = shaped.metrics.leading;
      return true;
    }
  }

  return false;
}

bool AppendEllipsis(
    const BLFontManager &font_mgr, Paragraph *paragraph, std::string *error) {
  if (paragraph->lines.empty() || paragraph->ellipsis.empty()) {
    return true;
  }

  Paragraph::Line &line = paragraph->lines.back();
  Paragraph::Style ellipsis_style = paragraph->base_style;
  if (!line.runs.empty()) {
    ellipsis_style.font = line.runs.back().font;
    ellipsis_style.color = line.runs.back().color;
  }

  PrefixFit ellipsis_fit;
  if (!ShapeLargestPrefixThatFits(font_mgr, ellipsis_style, paragraph->ellipsis,
          paragraph->layout_width, &ellipsis_fit, error)) {
    return false;
  }

  while (!line.runs.empty() &&
      line.width + ellipsis_fit.shaped.metrics.advance_width >
          paragraph->layout_width) {
    Paragraph::Run &last = line.runs.back();
    const double available = std::max(
        0.0, paragraph->layout_width - ellipsis_fit.shaped.metrics.advance_width -
                 (line.width - last.width));

    if (!last.whitespace && available > 0.0 &&
        TrimRunToFitEllipsis(font_mgr, &last, available, error)) {
      if (last.text.empty()) {
        line.runs.pop_back();
      }
      break;
    }

    line.runs.pop_back();
    RecomputeLine(&line, ellipsis_style);
  }

  RecomputeLine(&line, ellipsis_style);
  AddRun(&line, ellipsis_style,
      paragraph->ellipsis.substr(0, ellipsis_fit.bytes), false, line.end_index,
      line.end_index, true, std::move(ellipsis_fit.shaped));
  return true;
}

void FinalizeParagraphMetrics(Paragraph *paragraph) {
  const double min_intrinsic_width = paragraph->metrics.min_intrinsic_width;
  const double max_intrinsic_width = paragraph->metrics.max_intrinsic_width;
  const bool did_exceed_max_lines = paragraph->metrics.did_exceed_max_lines;

  paragraph->metrics = {};
  paragraph->metrics.width = paragraph->layout_width;
  paragraph->metrics.min_intrinsic_width = min_intrinsic_width;
  paragraph->metrics.max_intrinsic_width = max_intrinsic_width;
  paragraph->metrics.did_exceed_max_lines = did_exceed_max_lines;
  paragraph->metrics.line_count = paragraph->lines.size();
  paragraph->metrics.lines.reserve(paragraph->lines.size());

  double pen_y = 0.0;
  for (const Paragraph::Line &line : paragraph->lines) {
    pen_y += line.ascent;
    const double baseline = pen_y;
    const double height = line.ascent + line.descent + line.leading;
    const double left =
        AlignOffset(paragraph->align, paragraph->layout_width, line.width);
    pen_y += line.descent + line.leading;
    paragraph->metrics.longest_line =
        std::max(paragraph->metrics.longest_line, line.width);

    ParagraphLineMetrics line_metrics;
    line_metrics.start_index = line.start_index;
    line_metrics.end_index = line.end_index;
    line_metrics.ascent = line.ascent;
    line_metrics.descent = line.descent;
    line_metrics.height = height;
    line_metrics.width = line.width;
    line_metrics.left = left;
    line_metrics.baseline = baseline;
    paragraph->metrics.lines.push_back(line_metrics);
  }

  paragraph->metrics.height = pen_y;
  paragraph->metrics.line_height = paragraph->lines.empty()
      ? 0.0
      : paragraph->lines.front().ascent + paragraph->lines.front().descent +
            paragraph->lines.front().leading;
  paragraph->metrics.alphabetic_baseline = paragraph->metrics.lines.empty()
      ? 0.0
      : paragraph->metrics.lines.front().baseline;
  paragraph->metrics.ideographic_baseline = paragraph->metrics.lines.empty()
      ? 0.0
      : paragraph->metrics.lines.front().baseline +
            paragraph->metrics.lines.front().descent +
            paragraph->lines.front().leading;

  for (size_t i = 0; i < paragraph->lines.size(); ++i) {
    paragraph->lines[i].baseline = paragraph->metrics.lines[i].baseline;
    paragraph->lines[i].left = paragraph->metrics.lines[i].left;
  }
}

} // namespace

bool Paragraph::IsValid() const { return valid; }

std::string const &Paragraph::Error() const { return last_error; }

ParagraphMetrics Paragraph::Measure() const { return metrics; }

bool Paragraph::Init(Canvas const &canvas, Font const &font, uint32_t color,
    TextAlign align_, std::optional<size_t> max_lines_, const char *ellipsis_,
    std::string *error) {
  last_error.clear();
  if (!canvas.font_mgr.is_valid()) {
    last_error = "paragraph: canvas has no font manager";
    SetError(error, "paragraph: canvas has no font manager");
    valid = false;
    return false;
  }

  valid = false;
  font_mgr = canvas.font_mgr;
  base_style.font = font;
  base_style.color = color;
  align = align_;
  max_lines = max_lines_;
  ellipsis = ellipsis_ != nullptr ? ellipsis_ : "";
  last_error.clear();
  layout_width = 0.0;
  metrics = {};
  style_stack.clear();
  segments.clear();
  lines.clear();
  return true;
}

void Paragraph::PushStyle(Font const &font, uint32_t color) {
  style_stack.push_back(Style{font, color});
}

void Paragraph::PopStyle() {
  if (!style_stack.empty()) {
    style_stack.pop_back();
  }
}

void Paragraph::AddText(std::string_view text) {
  if (text.empty()) {
    return;
  }

  const Style style = style_stack.empty() ? base_style : style_stack.back();
  if (!segments.empty()) {
    Segment &last = segments.back();
    if (last.style.font.size == style.font.size &&
        last.style.font.family == style.font.family &&
        last.style.font.weight == style.font.weight &&
        last.style.font.width == style.font.width &&
        last.style.font.slant == style.font.slant &&
        last.style.color == style.color) {
      last.text.append(text.data(), text.size());
      return;
    }
  }

  Segment segment;
  segment.style = style;
  segment.text.assign(text.data(), text.size());
  segments.push_back(std::move(segment));
}

void Paragraph::Layout(double width) {
  layout_width = width;
  valid = false;
  lines.clear();
  metrics = {};
  last_error.clear();

  if (!(layout_width > 0.0)) {
    last_error = "paragraph: layout width must be positive";
    return;
  }

  if (segments.empty()) {
    Line empty_line;
    if (!SeedEmptyLine(font_mgr, &empty_line, base_style, &last_error)) {
      return;
    }
    lines.push_back(std::move(empty_line));
    FinalizeParagraphMetrics(this);
    valid = true;
    return;
  }

  Line current_line;
  current_line.start_index = 0;
  Style last_style = base_style;
  double natural_line_width = 0.0;
  bool ended_with_newline = false;
  bool suppress_leading_whitespace = false;
  size_t global_text_offset = 0;

  for (const Segment &segment : segments) {
    last_style = segment.style;
    std::string_view remaining(segment.text);
    const size_t segment_offset = global_text_offset;
    size_t index = 0;

    while (index < remaining.size()) {
      const unsigned char ch = static_cast<unsigned char>(remaining[index]);

      if (ch == '\n' || ch == '\r') {
        metrics.max_intrinsic_width =
            std::max(metrics.max_intrinsic_width, natural_line_width);
        current_line.end_index = segment_offset + index;
        if (!SeedEmptyLine(font_mgr, &current_line, segment.style, &last_error)) {
          return;
        }
        lines.push_back(std::move(current_line));
        current_line = {};
        current_line.start_index = segment_offset + index + 1;
        current_line.end_index = current_line.start_index;
        natural_line_width = 0.0;
        ended_with_newline = true;
        suppress_leading_whitespace = false;
        ++index;
        if (ch == '\r' && index < remaining.size() && remaining[index] == '\n') {
          ++index;
          current_line.start_index = segment_offset + index;
          current_line.end_index = current_line.start_index;
        }
        continue;
      }

      const bool whitespace = IsHorizontalWhitespace(ch);
      const size_t token_start = index;
      while (index < remaining.size()) {
        const unsigned char c = static_cast<unsigned char>(remaining[index]);
        if (c == '\n' || c == '\r') {
          break;
        }
        if (IsHorizontalWhitespace(c) != whitespace) {
          break;
        }
        ++index;
      }

      const std::string_view token =
          remaining.substr(token_start, index - token_start);
      const size_t token_global_start = segment_offset + token_start;
      const size_t token_global_end = segment_offset + index;
      if (token.empty()) {
        continue;
      }

      ShapedText shaped;
      if (!ShapeText(font_mgr, segment.style.font, token, "paragraph", &shaped,
              &last_error)) {
        return;
      }

      natural_line_width += shaped.metrics.advance_width;
      if (!whitespace) {
        metrics.min_intrinsic_width =
            std::max(metrics.min_intrinsic_width, shaped.metrics.advance_width);
      }

      if (whitespace && current_line.runs.empty() && suppress_leading_whitespace) {
        current_line.start_index = token_global_end;
        current_line.end_index = token_global_end;
        ended_with_newline = false;
        continue;
      }

      if (current_line.width + shaped.metrics.advance_width <= layout_width ||
          shaped.metrics.advance_width <= 0.0) {
        AddRun(&current_line, segment.style, token, whitespace, token_global_start,
            token_global_end, false, std::move(shaped));
        ended_with_newline = false;
        suppress_leading_whitespace = false;
        continue;
      }

      if (whitespace) {
        current_line.end_index = token_global_start;
        if (!SeedEmptyLine(font_mgr, &current_line, segment.style, &last_error)) {
          return;
        }
        lines.push_back(std::move(current_line));
        current_line = {};
        current_line.start_index = token_global_end;
        current_line.end_index = token_global_end;
        ended_with_newline = false;
        suppress_leading_whitespace = true;
        continue;
      }

      if (!current_line.runs.empty()) {
        lines.push_back(std::move(current_line));
        current_line = {};
        current_line.start_index = token_global_start;
        current_line.end_index = token_global_start;
        suppress_leading_whitespace = true;
      }

      std::string_view word = token;
      size_t word_start_index = token_global_start;
      while (!word.empty()) {
        PrefixFit fit;
        if (!ShapeLargestPrefixThatFits(
                font_mgr, segment.style, word, layout_width, &fit, &last_error)) {
          return;
        }

        AddRun(&current_line, segment.style, word.substr(0, fit.bytes), false,
            word_start_index, word_start_index + fit.bytes, false,
            std::move(fit.shaped));
        word.remove_prefix(fit.bytes);
        word_start_index += fit.bytes;
        suppress_leading_whitespace = false;

        if (!word.empty()) {
          lines.push_back(std::move(current_line));
          current_line = {};
          current_line.start_index = word_start_index;
          current_line.end_index = word_start_index;
          suppress_leading_whitespace = true;
        }
      }

      ended_with_newline = false;
    }

    global_text_offset += remaining.size();
  }

  metrics.max_intrinsic_width =
      std::max(metrics.max_intrinsic_width, natural_line_width);

  if (!current_line.runs.empty() || lines.empty() || ended_with_newline) {
    if (!SeedEmptyLine(font_mgr, &current_line, last_style, &last_error)) {
      return;
    }
    lines.push_back(std::move(current_line));
  }

  if (max_lines.has_value() && lines.size() > *max_lines) {
    lines.resize(*max_lines);
    metrics.did_exceed_max_lines = true;
    if (!AppendEllipsis(font_mgr, this, &last_error)) {
      return;
    }
  }

  FinalizeParagraphMetrics(this);
  valid = true;
}

bool Canvas::MeasureText(std::string_view text, Font const &font,
    Paint const * /*paint*/, TextMetrics *metrics, std::string *error) {
  ShapedText shaped;
  if (!ShapeText(font_mgr, font, text, "measure_text", &shaped, error)) {
    return false;
  }
  *metrics = shaped.metrics;
  return true;
}

bool Canvas::DrawText(std::string_view text, double x, double y,
    Font const &font, Paint const *paint, std::string *error) {
  ShapedText shaped;
  if (!ShapeText(font_mgr, font, text, "draw_text", &shaped, error)) {
    return false;
  }

  Path path;
  path.bl = std::move(shaped.path);
  path.bl.translate(BLPoint(x, y));
  DrawPath(path, const_cast<Paint *>(paint));
  return true;
}

void Canvas::DrawParagraph(Paragraph const &paragraph, double x, double y) {
  for (const Paragraph::Line &line : paragraph.lines) {
    for (const Paragraph::Run &run : line.runs) {
      Paint paint;
      Shader shader;
      shader.MakeSolidColor(run.color);
      paint.SetShader(shader);

      Path path;
      path.bl = run.path;
      path.bl.translate(BLPoint(x + line.left + run.x, y + line.baseline));
      DrawPath(path, &paint);
    }
  }
}

} // namespace luna::backend::blend2d
