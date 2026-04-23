#include "skia/canvas.h"

#include <memory>
#include <vector>

#include <skia/core/SkString.h>
#include <skia/modules/skparagraph/include/FontCollection.h>
#include <skia/modules/skparagraph/include/Metrics.h>
#include <skia/modules/skparagraph/include/Paragraph.h>
#include <skia/modules/skparagraph/include/ParagraphBuilder.h>
#include <skia/modules/skparagraph/include/ParagraphStyle.h>
#include <skia/modules/skparagraph/include/TextStyle.h>
#include <skia/modules/skunicode/include/SkUnicode_icu.h>

namespace luna::backend::skia {

using ::skia::textlayout::FontCollection;
using ::skia::textlayout::LineMetrics;
using ::skia::textlayout::ParagraphBuilder;
using ::skia::textlayout::ParagraphStyle;
using ::skia::textlayout::TextStyle;

namespace {

TextStyle ResolveTextStyle(Canvas::Font const &font, uint32_t color) {
  TextStyle text_style;
  text_style.setColor(static_cast<SkColor>(color));
  text_style.setFontSize(font.size);
  text_style.setFontFamilies({font.family_name});
  text_style.setFontStyle(font.style);
  return text_style;
}

} // namespace

bool Paragraph::IsValid() const { return static_cast<bool>(sk); }

std::string const &Paragraph::Error() const { return last_error; }

ParagraphMetrics Paragraph::Measure() const {
  ParagraphMetrics metrics;
  metrics.width = sk->getMaxWidth();
  metrics.height = sk->getHeight();
  metrics.longest_line = sk->getLongestLine();
  metrics.min_intrinsic_width = sk->getMinIntrinsicWidth();
  metrics.max_intrinsic_width = sk->getMaxIntrinsicWidth();
  metrics.alphabetic_baseline = sk->getAlphabeticBaseline();
  metrics.ideographic_baseline = sk->getIdeographicBaseline();
  metrics.did_exceed_max_lines = sk->didExceedMaxLines();

  std::vector<LineMetrics> lines;
  sk->getLineMetrics(lines);
  metrics.line_count = lines.size();
  metrics.line_height = lines.empty() ? 0.0 : lines.front().fHeight;
  metrics.lines.reserve(lines.size());
  for (const LineMetrics &line : lines) {
    ParagraphLineMetrics line_metrics;
    line_metrics.start_index = line.fStartIndex;
    line_metrics.end_index = line.fEndIndex;
    line_metrics.ascent = line.fAscent;
    line_metrics.descent = line.fDescent;
    line_metrics.height = line.fHeight;
    line_metrics.width = line.fWidth;
    line_metrics.left = line.fLeft;
    line_metrics.baseline = line.fBaseline;
    metrics.lines.push_back(line_metrics);
  }
  return metrics;
}

bool Paragraph::Init(Canvas const &canvas, Canvas::Font const &font,
    uint32_t color, TextAlign align, std::optional<size_t> max_lines,
    const char *ellipsis, std::string *error) {
  last_error.clear();
  if (!canvas.font_mgr()) {
    last_error = "paragraph: canvas has no font manager";
    *error = "paragraph: canvas has no font manager";
    return false;
  }

  font_collection = sk_make_sp<FontCollection>();
  font_collection->setDefaultFontManager(canvas.font_mgr());

  TextStyle text_style = ResolveTextStyle(font, color);

  ParagraphStyle paragraph_style;
  paragraph_style.setTextAlign(align);
  paragraph_style.setTextStyle(text_style);
  if (max_lines.has_value()) {
    paragraph_style.setMaxLines(*max_lines);
  }
  if (ellipsis != nullptr) {
    paragraph_style.setEllipsis(SkString(ellipsis));
  }

  sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();
  if (!unicode) {
    last_error = "paragraph: failed to initialize unicode support";
    *error = "paragraph: failed to initialize unicode support";
    return false;
  }

  sk.reset();

  sk_builder =
      ParagraphBuilder::make(paragraph_style, font_collection, unicode);
  if (!sk_builder) {
    last_error = "paragraph: failed to create builder";
    *error = "paragraph: failed to create builder";
    return false;
  }

  return true;
}

void Paragraph::PushStyle(Canvas::Font const &font, uint32_t color) {
  if (sk_builder) {
    sk_builder->pushStyle(ResolveTextStyle(font, color));
  }
}

void Paragraph::PopStyle() {
  if (sk_builder) {
    sk_builder->pop();
  }
}

void Paragraph::AddText(std::string_view text) {
  if (sk_builder) {
    sk_builder->addText(text.data(), text.size());
  }
}

void Paragraph::Layout(double width) {
  last_error.clear();
  layout_width = width;
  if (sk_builder) {
    sk = sk_builder->Build();
    sk->layout(SkDoubleToScalar(layout_width));
  }
}

void Canvas::DrawParagraph(const Paragraph &paragraph, double x, double y) {
  paragraph.sk->paint(sk_, SkFloatToScalar(static_cast<float>(x)),
      SkFloatToScalar(static_cast<float>(y)));
}

} // namespace luna::backend::skia
