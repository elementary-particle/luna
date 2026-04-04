#include "canvas.h"

#include <cstring>
#include <limits>
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

namespace luna::canvas {

namespace {

skia::textlayout::TextAlign CheckTextAlign(lua_State *L, int idx) {
  return static_cast<skia::textlayout::TextAlign>(luaL_checkinteger(L, idx));
}

} // namespace

using skia::textlayout::FontCollection;
using skia::textlayout::ParagraphBuilder;
using skia::textlayout::ParagraphStyle;
using skia::textlayout::TextStyle;

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

void RegisterParagraphBindings(lua_State *L) {
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
}

void RegisterParagraphCanvasMethods(lua_State *L) {
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

    LCanvas::LFont *font = nullptr;
    if (!lua_isnoneornil(L, 4)) {
      font = lua::Check<LCanvas::LFont>(L, 4);
    }

    SkColor color = SK_ColorWHITE;
    if (!lua_isnoneornil(L, 5)) {
      color = static_cast<SkColor>(luaL_checkinteger(L, 5));
    }

    auto align = skia::textlayout::TextAlign::kLeft;
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
    text_style.setFontSize(font ? font->size : SkFloatToScalar(14.0f));

    if (font && font->has_family) {
      text_style.setFontFamilies({font->family_name});
      text_style.setFontStyle(font->style);
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
    lua::New<LParagraph>(
        L, std::move(font_collection), std::move(paragraph), layout_width);
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
    paragraph->sk->paint(canvas->sk(), SkFloatToScalar(x), SkFloatToScalar(y));
    return 0;
  });
  lua_setfield(L, -2, "_draw_paragraph");
}

void RegisterParagraphLuaHelpers(lua_State *L) {
  static constexpr char PARAGRAPH_HELPERS[] = R"(
return function(canvas_mt)
  local raw_paragraph = assert(canvas_mt._paragraph)
  local raw_draw_paragraph = assert(canvas_mt._draw_paragraph)

  local constants = assert(canvas_mt._constants)

  local text_align_ = {
    left = constants.text_align.left,
    center = constants.text_align.center,
    right = constants.text_align.right,
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
      self:font(opts.font),
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

  if (luaL_loadbuffer(L, PARAGRAPH_HELPERS, std::strlen(PARAGRAPH_HELPERS),
                      "canvas_paragraph_helpers") != 0) {
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

} // namespace luna::canvas
