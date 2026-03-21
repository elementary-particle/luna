#include "canvas.h"

#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <skia/core/SkFont.h>
#include <skia/core/SkFontMetrics.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkRefCnt.h>
#include <skia/core/SkSamplingOptions.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTypeface.h>
#include <skia/core/SkTypes.h>
#include <skia/modules/skparagraph/include/DartTypes.h>
#include <skia/modules/skparagraph/include/FontCollection.h>
#include <skia/modules/skparagraph/include/Metrics.h>
#include <skia/modules/skparagraph/include/ParagraphBuilder.h>
#include <skia/modules/skparagraph/include/ParagraphStyle.h>
#include <skia/modules/skparagraph/include/TextStyle.h>
#include <skia/modules/skunicode/include/SkUnicode_icu.h>

namespace luna {

namespace {

using skia::textlayout::FontCollection;
using skia::textlayout::ParagraphBuilder;
using skia::textlayout::ParagraphStyle;
using skia::textlayout::TextAlign;
using skia::textlayout::TextStyle;

SkPaint::Style CheckPaintStyle(lua_State *L, int idx) {
  return static_cast<SkPaint::Style>(luaL_checkinteger(L, idx));
}

TextAlign CheckTextAlign(lua_State *L, int idx) {
  return static_cast<TextAlign>(luaL_checkinteger(L, idx));
}

void PushTextMetricsTable(lua_State *L,
                          SkScalar advance_width,
                          const SkRect &bounds,
                          const SkFontMetrics &metrics) {
  lua_createtable(L, 0, 9);
  lua_pushnumber(L, advance_width);
  lua_setfield(L, -2, "advance_width");
  lua_pushnumber(L, bounds.x());
  lua_setfield(L, -2, "bounds_x");
  lua_pushnumber(L, bounds.y());
  lua_setfield(L, -2, "bounds_y");
  lua_pushnumber(L, bounds.width());
  lua_setfield(L, -2, "bounds_w");
  lua_pushnumber(L, bounds.height());
  lua_setfield(L, -2, "bounds_h");
  lua_pushnumber(L, metrics.fAscent);
  lua_setfield(L, -2, "ascent");
  lua_pushnumber(L, metrics.fDescent);
  lua_setfield(L, -2, "descent");
  lua_pushnumber(L, metrics.fLeading);
  lua_setfield(L, -2, "leading");
  lua_pushnumber(L, metrics.fDescent - metrics.fAscent + metrics.fLeading);
  lua_setfield(L, -2, "line_height");
}

void PushParagraphMetricsTable(lua_State *L, LParagraph *paragraph) {
  lua_createtable(L, 0, 8);
  lua_pushnumber(L, paragraph->layout_width);
  lua_setfield(L, -2, "width");
  lua_pushnumber(L, paragraph->sk->getHeight());
  lua_setfield(L, -2, "height");
  lua_pushnumber(L, paragraph->sk->getLongestLine());
  lua_setfield(L, -2, "longest_line");
  lua_pushnumber(L, paragraph->sk->getMinIntrinsicWidth());
  lua_setfield(L, -2, "min_intrinsic_width");
  lua_pushnumber(L, paragraph->sk->getMaxIntrinsicWidth());
  lua_setfield(L, -2, "max_intrinsic_width");
  lua_pushboolean(L, paragraph->sk->didExceedMaxLines());
  lua_setfield(L, -2, "did_exceed_max_lines");

  std::vector<skia::textlayout::LineMetrics> lines;
  paragraph->sk->getLineMetrics(lines);
  lua_pushinteger(L, static_cast<lua_Integer>(lines.size()));
  lua_setfield(L, -2, "line_count");

  lua_pushnumber(L, lines.empty() ? 0.0 : lines.front().fHeight);
  lua_setfield(L, -2, "line_height");
}

SkString MakeParagraphFontFamily(const SkFont &font) {
  sk_sp<SkTypeface> typeface = font.refTypeface();
  if (!typeface) {
    return SkString(DEFAULT_FONT_FAMILY);
  }

  SkString family_name;
  typeface->getFamilyName(&family_name);
  if (family_name.isEmpty()) {
    family_name = SkString("luna-user-font");
  }
  return family_name;
}

} // namespace

void LCanvas::RegisterLuaHelpers(lua_State *L) {
  static constexpr char CANVAS_HELPERS[] = R"(
return function(canvas_mt)
  local raw_paint = assert(canvas_mt._paint)
  local raw_font = assert(canvas_mt._font)
  local raw_draw_rect = assert(canvas_mt._draw_rect)
  local raw_draw_image_rect = assert(canvas_mt._draw_image_rect)
  local raw_draw_text = assert(canvas_mt._draw_text)
  local raw_measure_text = assert(canvas_mt._measure_text)
  local raw_paragraph = assert(canvas_mt._paragraph)
  local raw_draw_paragraph = assert(canvas_mt._draw_paragraph)

  local constants = assert(canvas_mt._constants)

  local paint_style_ = {
    fill = constants.paint_style.fill,
    stroke = constants.paint_style.stroke,
    stroke_and_fill = constants.paint_style.stroke_and_fill,
    fill_and_stroke = constants.paint_style.stroke_and_fill,
  }

  local text_align_ = {
    left = constants.text_align.left,
    center = constants.text_align.center,
    right = constants.text_align.right,
  }

  local allowed_paint_keys = {
    anti_alias = true,
    color = true,
    stroke_width = true,
    style = true,
  }

  local allowed_font_keys = {
    size = true,
    typeface = true,
  }

  local allowed_paragraph_keys = {
    text = true,
    width = true,
    font = true,
    color = true,
    align = true,
    max_lines = true,
    ellipsis = true,
  }

  local function normalize_paint_style(style, level)
    if style == nil then
      return nil
    end
    if type(style) == "number" then
      return style
    end
    if type(style) ~= "string" then
      error("paint.style must be a number or string", level or 3)
    end

    local mapped = paint_style_[style]
    if mapped == nil then
      error(string.format("invalid paint.style '%s'", style), level or 3)
    end
    return mapped
  end

  local function normalize_text_align(align, level)
    if align == nil then
      return nil
    end
    if type(align) == "number" then
      return align
    end
    if type(align) ~= "string" then
      error("paragraph.align must be a number or string", level or 3)
    end

    local mapped = text_align_[align]
    if mapped == nil then
      error(string.format("invalid paragraph.align '%s'", align), level or 3)
    end
    return mapped
  end

  local function compile_paint(self, paint, level)
    if paint == nil or type(paint) == "userdata" then
      return paint
    end
    if type(paint) ~= "table" then
      error("paint must be a table or compiled paint", level or 3)
    end

    for key in pairs(paint) do
      if not allowed_paint_keys[key] then
        error(string.format("unknown paint field '%s'", tostring(key)), level or 3)
      end
    end

    return raw_paint(
      self,
      paint.color,
      paint.anti_alias,
      normalize_paint_style(paint.style, (level or 3) + 1),
      paint.stroke_width
    )
  end

  local function compile_font(self, font, level)
    if font == nil or type(font) == "userdata" then
      return font
    end
    if type(font) ~= "table" then
      error("font must be a table or compiled font", level or 3)
    end

    for key in pairs(font) do
      if not allowed_font_keys[key] then
        error(string.format("unknown font field '%s'", tostring(key)), level or 3)
      end
    end

    if font.size == nil then
      error("font.size is required", level or 3)
    end

    return raw_font(self, font.size, font.typeface)
  end

  function canvas_mt:paint(paint)
    return compile_paint(self, paint, 3)
  end

  function canvas_mt:font(font)
    return compile_font(self, font, 3)
  end

  function canvas_mt:draw_rect(x, y, w, h, paint)
    return raw_draw_rect(self, x, y, w, h, compile_paint(self, paint, 3))
  end

  function canvas_mt:draw_image_rect(image, x, y, w, h, sx, sy, sw, sh, paint)
    return raw_draw_image_rect(
      self,
      image,
      x,
      y,
      w,
      h,
      sx,
      sy,
      sw,
      sh,
      compile_paint(self, paint, 3)
    )
  end

  function canvas_mt:draw_text(text, x, y, font, paint)
    return raw_draw_text(
      self,
      text,
      x,
      y,
      compile_font(self, font, 3),
      compile_paint(self, paint, 3)
    )
  end

  function canvas_mt:measure_text(text, font, paint)
    return raw_measure_text(
      self,
      text,
      compile_font(self, font, 3),
      compile_paint(self, paint, 3)
    )
  end

  function canvas_mt:paragraph(opts)
    if type(opts) ~= "table" then
      error("paragraph options must be a table", 2)
    end

    for key in pairs(opts) do
      if not allowed_paragraph_keys[key] then
        error(string.format("unknown paragraph field '%s'", tostring(key)), 2)
      end
    end

    if opts.text == nil then
      error("paragraph.text is required", 2)
    end

    if opts.width == nil then
      error("paragraph.width is required", 2)
    end

    return raw_paragraph(
      self,
      opts.text,
      opts.width,
      compile_font(self, opts.font, 3),
      opts.color,
      normalize_text_align(opts.align, 3),
      opts.max_lines,
      opts.ellipsis
    )
  end

  function canvas_mt:draw_paragraph(paragraph, x, y)
    return raw_draw_paragraph(self, paragraph, x, y)
  end
end
)";

  if (luaL_loadbuffer(
          L, CANVAS_HELPERS, std::strlen(CANVAS_HELPERS), "canvas_helpers") !=
      0) {
    lua_error(L);
  }

  if (lua_pcall(L, 0, 1, 0) != 0) {
    lua_error(L);
  }

  lua_pushvalue(L, -2);
  if (lua_pcall(L, 1, 0, 0) != 0) {
    lua_error(L);
  }
}

void LCanvas::RegisterBindings(lua_State *L) {
  lua::NewType<LPaint>(L);
  lua_pop(L, 1);
  lua::NewType<LFont>(L);
  lua_pop(L, 1);
  if (lua::NewType<LParagraph>(L)) {
    lua::PushFunction(L, [](lua_State *L) {
      LParagraph *paragraph = lua::Check<LParagraph>(L, 1);
      if (!paragraph->sk) {
        return luaL_error(L, "measure: paragraph is empty");
      }
      PushParagraphMetricsTable(L, paragraph);
      return 1;
    });
    lua_setfield(L, -2, "measure");
  }
  lua_pop(L, 1);

  if (lua::NewType<LCanvas>(L)) {
    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      SkColor color = static_cast<SkColor>(luaL_checkinteger(L, 2));
      canvas->sk_->clear(color);
      return 0;
    });
    lua_setfield(L, -2, "clear");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      canvas->sk_->save();
      return 0;
    });
    lua_setfield(L, -2, "save");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      canvas->sk_->restore();
      return 0;
    });
    lua_setfield(L, -2, "restore");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      float dx = static_cast<float>(luaL_checknumber(L, 2));
      float dy = static_cast<float>(luaL_checknumber(L, 3));
      canvas->sk_->translate(SkFloatToScalar(dx), SkFloatToScalar(dy));
      return 0;
    });
    lua_setfield(L, -2, "translate");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      float sx = static_cast<float>(luaL_checknumber(L, 2));
      float sy = static_cast<float>(luaL_checknumber(L, 3));
      canvas->sk_->scale(SkFloatToScalar(sx), SkFloatToScalar(sy));
      return 0;
    });
    lua_setfield(L, -2, "scale");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      float degrees = static_cast<float>(luaL_checknumber(L, 2));
      canvas->sk_->rotate(SkFloatToScalar(degrees));
      return 0;
    });
    lua_setfield(L, -2, "rotate");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      if (!canvas->surface_) {
        return luaL_error(L, "snapshot: canvas does not support snapshots");
      }

      sk_sp<SkImage> image = canvas->surface_->makeImageSnapshot();
      if (!image) {
        return luaL_error(L, "snapshot: failed to create image");
      }

      lua::New<LImage>(L, std::move(image));
      return 1;
    });
    lua_setfield(L, -2, "snapshot");

    lua_newtable(L);

    lua_newtable(L);

    lua_pushinteger(L, SkPaint::kFill_Style);
    lua_setfield(L, -2, "fill");

    lua_pushinteger(L, SkPaint::kStroke_Style);
    lua_setfield(L, -2, "stroke");

    lua_pushinteger(L, SkPaint::kStrokeAndFill_Style);
    lua_setfield(L, -2, "stroke_and_fill");

    lua_setfield(L, -2, "paint_style");

    lua_newtable(L);

    lua_pushinteger(L, static_cast<lua_Integer>(TextAlign::kLeft));
    lua_setfield(L, -2, "left");

    lua_pushinteger(L, static_cast<lua_Integer>(TextAlign::kCenter));
    lua_setfield(L, -2, "center");

    lua_pushinteger(L, static_cast<lua_Integer>(TextAlign::kRight));
    lua_setfield(L, -2, "right");

    lua_setfield(L, -2, "text_align");

    lua_setfield(L, -2, "_constants");

    lua::PushFunction(L, [](lua_State *L) {
      lua::Check<LCanvas>(L, 1);

      auto *paint = lua::New<LPaint>(L);
      if (!lua_isnoneornil(L, 2)) {
        paint->sk.setColor(static_cast<SkColor>(luaL_checkinteger(L, 2)));
      }
      if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        paint->sk.setAntiAlias(lua_toboolean(L, 3) != 0);
      }
      if (!lua_isnoneornil(L, 4)) {
        paint->sk.setStyle(CheckPaintStyle(L, 4));
      }
      if (!lua_isnoneornil(L, 5)) {
        paint->sk.setStrokeWidth(
            SkFloatToScalar(static_cast<float>(luaL_checknumber(L, 5))));
      }
      return 1;
    });
    lua_setfield(L, -2, "_paint");

    lua::PushFunction(L, [](lua_State *L) {
      lua::Check<LCanvas>(L, 1);

      auto *font = lua::New<LFont>(L);
      font->sk.setSize(
          SkFloatToScalar(static_cast<float>(luaL_checknumber(L, 2))));
      if (!lua_isnoneornil(L, 3)) {
        LTypeface *typeface = lua::Check<LTypeface>(L, 3);
        font->sk.setTypeface(typeface->sk);
      }
      return 1;
    });
    lua_setfield(L, -2, "_font");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      float x = static_cast<float>(luaL_checknumber(L, 2));
      float y = static_cast<float>(luaL_checknumber(L, 3));
      float w = static_cast<float>(luaL_checknumber(L, 4));
      float h = static_cast<float>(luaL_checknumber(L, 5));

      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 6)) {
        paint = lua::Check<LPaint>(L, 6);
      }

      SkPaint default_paint;
      canvas->sk_->drawRect(SkRect::MakeXYWH(SkFloatToScalar(x),
                                             SkFloatToScalar(y),
                                             SkFloatToScalar(w),
                                             SkFloatToScalar(h)),
                            paint ? paint->sk : default_paint);
      return 0;
    });
    lua_setfield(L, -2, "_draw_rect");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      LImage *image = lua::Check<LImage>(L, 2);
      float x = static_cast<float>(luaL_checknumber(L, 3));
      float y = static_cast<float>(luaL_checknumber(L, 4));
      float w = static_cast<float>(luaL_checknumber(L, 5));
      float h = static_cast<float>(luaL_checknumber(L, 6));
      float sx = static_cast<float>(luaL_checknumber(L, 7));
      float sy = static_cast<float>(luaL_checknumber(L, 8));
      float sw = static_cast<float>(luaL_checknumber(L, 9));
      float sh = static_cast<float>(luaL_checknumber(L, 10));

      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 11)) {
        paint = lua::Check<LPaint>(L, 11);
      }

      SkPaint default_paint;
      canvas->sk_->drawImageRect(
          image->sk,
          SkRect::MakeXYWH(SkFloatToScalar(sx),
                           SkFloatToScalar(sy),
                           SkFloatToScalar(sw),
                           SkFloatToScalar(sh)),
          SkRect::MakeXYWH(SkFloatToScalar(x),
                           SkFloatToScalar(y),
                           SkFloatToScalar(w),
                           SkFloatToScalar(h)),
          SkSamplingOptions(),
          paint ? &paint->sk : &default_paint,
          SkCanvas::SrcRectConstraint::kStrict_SrcRectConstraint);
      return 0;
    });
    lua_setfield(L, -2, "_draw_image_rect");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      size_t text_len = 0;
      const char *text = luaL_checklstring(L, 2, &text_len);
      float x = static_cast<float>(luaL_checknumber(L, 3));
      float y = static_cast<float>(luaL_checknumber(L, 4));
      LFont *font = lua::Check<LFont>(L, 5);

      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 6)) {
        paint = lua::Check<LPaint>(L, 6);
      }

      SkPaint default_paint;
      canvas->sk_->drawSimpleText(text,
                                  text_len,
                                  SkTextEncoding::kUTF8,
                                  SkFloatToScalar(x),
                                  SkFloatToScalar(y),
                                  font->sk,
                                  paint ? paint->sk : default_paint);
      return 0;
    });
    lua_setfield(L, -2, "_draw_text");

    lua::PushFunction(L, [](lua_State *L) {
      lua::Check<LCanvas>(L, 1);
      size_t text_len = 0;
      const char *text = luaL_checklstring(L, 2, &text_len);
      LFont *font = lua::Check<LFont>(L, 3);

      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 4)) {
        paint = lua::Check<LPaint>(L, 4);
      }

      SkRect bounds = SkRect::MakeEmpty();
      SkFontMetrics metrics;
      const SkScalar advance_width =
          font->sk.measureText(text,
                               text_len,
                               SkTextEncoding::kUTF8,
                               &bounds,
                               paint ? &paint->sk : nullptr);
      font->sk.getMetrics(&metrics);
      PushTextMetricsTable(L, advance_width, bounds, metrics);
      return 1;
    });
    lua_setfield(L, -2, "_measure_text");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      size_t text_len = 0;
      const char *text = luaL_checklstring(L, 2, &text_len);
      const float width = static_cast<float>(luaL_checknumber(L, 3));

      if (!(width > 0.0f)) {
        return luaL_error(L, "paragraph.width must be positive");
      }
      if (!canvas->font_mgr()) {
        return luaL_error(L, "paragraph: canvas has no font manager");
      }

      LFont *font = nullptr;
      if (!lua_isnoneornil(L, 4)) {
        font = lua::Check<LFont>(L, 4);
      }

      SkColor color = SK_ColorWHITE;
      if (!lua_isnoneornil(L, 5)) {
        color = static_cast<SkColor>(luaL_checkinteger(L, 5));
      }

      TextAlign align = TextAlign::kLeft;
      if (!lua_isnoneornil(L, 6)) {
        align = CheckTextAlign(L, 6);
      }

      size_t max_lines = std::numeric_limits<size_t>::max();
      if (!lua_isnoneornil(L, 7)) {
        const lua_Integer lines = luaL_checkinteger(L, 7);
        if (lines <= 0) {
          return luaL_error(L, "paragraph.max_lines must be positive");
        }
        max_lines = static_cast<size_t>(lines);
      }

      const char *ellipsis = nullptr;
      if (!lua_isnoneornil(L, 8)) {
        ellipsis = luaL_checkstring(L, 8);
      }

      auto font_collection = sk_make_sp<FontCollection>();
      font_collection->setDefaultFontManager(canvas->font_mgr());

      TextStyle text_style;
      text_style.setColor(color);
      text_style.setFontSize(
          font ? font->sk.getSize() : SkFloatToScalar(14.0f));

      if (font && font->sk.refTypeface()) {
        const SkString family_name = MakeParagraphFontFamily(font->sk);
        text_style.setFontFamilies({family_name});
        text_style.setFontStyle(font->sk.refTypeface()->fontStyle());
      } else {
        text_style.setFontFamilies({SkString(DEFAULT_FONT_FAMILY)});
      }

      ParagraphStyle paragraph_style;
      paragraph_style.setTextAlign(align);
      paragraph_style.setTextStyle(text_style);
      if (max_lines != std::numeric_limits<size_t>::max()) {
        paragraph_style.setMaxLines(max_lines);
      }
      if (ellipsis != nullptr) {
        paragraph_style.setEllipsis(SkString(ellipsis));
      }

      sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();
      if (!unicode) {
        return luaL_error(L, "paragraph: failed to initialize unicode support");
      }

      std::unique_ptr<ParagraphBuilder> builder =
          ParagraphBuilder::make(paragraph_style, font_collection, unicode);
      if (!builder) {
        return luaL_error(L, "paragraph: failed to create builder");
      }

      builder->pushStyle(text_style);
      builder->addText(text, text_len);
      std::unique_ptr<skia::textlayout::Paragraph> paragraph = builder->Build();
      if (!paragraph) {
        return luaL_error(L, "paragraph: failed to build paragraph");
      }

      const SkScalar layout_width = SkFloatToScalar(width);
      paragraph->layout(layout_width);
      lua::New<LParagraph>(L,
                           std::move(font_collection),
                           std::move(paragraph),
                           layout_width);
      return 1;
    });
    lua_setfield(L, -2, "_paragraph");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      LParagraph *paragraph = lua::Check<LParagraph>(L, 2);
      const float x = static_cast<float>(luaL_checknumber(L, 3));
      const float y = static_cast<float>(luaL_checknumber(L, 4));
      if (!paragraph->sk) {
        return luaL_error(L, "draw_paragraph: paragraph is empty");
      }
      paragraph->sk->paint(
          canvas->sk_, SkFloatToScalar(x), SkFloatToScalar(y));
      return 0;
    });
    lua_setfield(L, -2, "_draw_paragraph");

    RegisterLuaHelpers(L);
  }
  lua_pop(L, 1);
}

} // namespace luna
