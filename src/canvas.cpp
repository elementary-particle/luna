#include "canvas.h"

#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <skia/core/SkClipOp.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMetrics.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkPath.h>
#include <skia/core/SkPathBuilder.h>
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
#include <skia/utils/SkParsePath.h>

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

SkPathFillType CheckPathFillType(lua_State *L, int idx) {
  return static_cast<SkPathFillType>(luaL_checkinteger(L, idx));
}

SkClipOp CheckClipOp(lua_State *L, int idx) {
  return static_cast<SkClipOp>(luaL_checkinteger(L, idx));
}

SkPathDirection CheckPathDirection(lua_State *L, int idx) {
  return static_cast<SkPathDirection>(luaL_checkinteger(L, idx));
}

TextAlign CheckTextAlign(lua_State *L, int idx) {
  return static_cast<TextAlign>(luaL_checkinteger(L, idx));
}

void PushTextMetricsTable(lua_State *L, SkScalar advance_width,
                          const SkRect &bounds, const SkFontMetrics &metrics) {
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

SkFont ResolveFont(lua_State *L, LCanvas *canvas, const LCanvas::LFont &font,
                   const char *operation) {
  SkFont resolved;
  resolved.setSize(font.size);

  if (!font.has_family) {
    return resolved;
  }
  if (!canvas->font_mgr()) {
    luaL_error(L, "%s: canvas has no font manager", operation);
  }

  sk_sp<SkTypeface> typeface = canvas->font_mgr()->matchFamilyStyle(
      font.family_name.c_str(), font.style);
  if (!typeface) {
    luaL_error(L,
               "%s: failed to resolve family '%s'",
               operation,
               font.family_name.c_str());
  }

  resolved.setTypeface(std::move(typeface));
  return resolved;
}

} // namespace

void LCanvas::RegisterLuaHelpers(lua_State *L) {
  static constexpr char CANVAS_HELPERS[] = R"(
return function(canvas_mt, path_mt)
  local raw_paint = assert(canvas_mt._paint)
  local raw_font = assert(canvas_mt._font)
  local raw_path = assert(canvas_mt._path)
  local raw_path_svg = assert(canvas_mt._path_svg)
  local raw_draw_rect = assert(canvas_mt._draw_rect)
  local raw_draw_path = assert(canvas_mt._draw_path)
  local raw_draw_image_rect = assert(canvas_mt._draw_image_rect)
  local raw_draw_text = assert(canvas_mt._draw_text)
  local raw_measure_text = assert(canvas_mt._measure_text)
  local raw_paragraph = assert(canvas_mt._paragraph)
  local raw_draw_paragraph = assert(canvas_mt._draw_paragraph)
  local raw_clip_rect = assert(canvas_mt._clip_rect)
  local raw_clip_path = assert(canvas_mt._clip_path)
  local raw_path_set_fill_type = assert(path_mt._set_fill_type)
  local raw_path_add_rect = assert(path_mt._add_rect)
  local raw_path_add_oval = assert(path_mt._add_oval)

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

  local path_fill_type_ = {
    winding = constants.path_fill_type.winding,
    even_odd = constants.path_fill_type.even_odd,
    inverse_winding = constants.path_fill_type.inverse_winding,
    inverse_even_odd = constants.path_fill_type.inverse_even_odd,
  }

  local clip_op_ = {
    difference = constants.clip_op.difference,
    intersect = constants.clip_op.intersect,
  }

  local allowed_paint_keys = {
    anti_alias = true,
    color = true,
    stroke_width = true,
    style = true,
  }

  local allowed_font_keys = {
    size = true,
    family = true,
    style = true,
    weight = true,
    width = true,
    slant = true,
  }

  local allowed_font_style_keys = {
    weight = true,
    width = true,
    slant = true,
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

  local allowed_path_keys = {
    svg = true,
    fill_type = true,
  }

  local path_direction_ = {
    cw = constants.path_direction.cw,
    clockwise = constants.path_direction.cw,
    ccw = constants.path_direction.ccw,
    counter_clockwise = constants.path_direction.ccw,
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

  local function normalize_path_fill_type(fill_type, level)
    if fill_type == nil then
      return nil
    end
    if type(fill_type) == "number" then
      return fill_type
    end
    if type(fill_type) ~= "string" then
      error("path.fill_type must be a number or string", level or 3)
    end

    local mapped = path_fill_type_[fill_type]
    if mapped == nil then
      error(string.format("invalid path.fill_type '%s'", fill_type), level or 3)
    end
    return mapped
  end

  local function normalize_clip_op(op, level)
    if op == nil then
      return nil
    end
    if type(op) == "number" then
      return op
    end
    if type(op) ~= "string" then
      error("clip op must be a number or string", level or 3)
    end

    local mapped = clip_op_[op]
    if mapped == nil then
      error(string.format("invalid clip op '%s'", op), level or 3)
    end
    return mapped
  end

  local function normalize_path_direction(direction, level)
    if direction == nil then
      return nil
    end
    if type(direction) == "number" then
      return direction
    end
    if type(direction) ~= "string" then
      error("path direction must be a number or string", level or 3)
    end

    local mapped = path_direction_[direction]
    if mapped == nil then
      error(string.format("invalid path direction '%s'", direction), level or 3)
    end
    return mapped
  end

  local function normalize_font_style_value(kind, value, map, level)
    if value == nil then
      return nil
    end
    if type(value) == "number" then
      return value
    end
    if type(value) ~= "string" then
      error(string.format("font.%s must be a number or string", kind), level or 3)
    end

    local mapped = map[value]
    if mapped == nil then
      error(string.format("invalid font.%s '%s'", kind, value), level or 3)
    end
    return mapped
  end

  local function compile_font_style(font, level)
    local style = font.style
    if style ~= nil then
      if type(style) ~= "table" then
        error("font.style must be a table", level or 3)
      end
      if font.weight ~= nil or font.width ~= nil or font.slant ~= nil then
        error("font.style cannot be combined with font.weight/font.width/font.slant", level or 3)
      end
      for key in pairs(style) do
        if not allowed_font_style_keys[key] then
          error(string.format("unknown font.style field '%s'", tostring(key)), level or 3)
        end
      end
      font = style
    end

    return {
      weight = normalize_font_style_value("weight", font.weight, constants.font_style.weight, (level or 3) + 1),
      width = normalize_font_style_value("width", font.width, constants.font_style.width, (level or 3) + 1),
      slant = normalize_font_style_value("slant", font.slant, constants.font_style.slant, (level or 3) + 1),
    }
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

    local style = compile_font_style(font, level)
    if font.family == nil and (style.weight ~= nil or style.width ~= nil or style.slant ~= nil) then
      error("font.family is required when specifying font style", level or 3)
    end

    return raw_font(self, font.size, font.family, style.weight, style.width, style.slant)
  end

  local function compile_path(self, path, level)
    if path == nil then
      return raw_path(self, nil)
    end
    if type(path) == "userdata" then
      return path
    end
    if type(path) == "string" then
      return raw_path_svg(self, path, nil)
    end
    if type(path) ~= "table" then
      error("path must be a table, SVG path string, or compiled path", level or 3)
    end

    for key in pairs(path) do
      if not allowed_path_keys[key] then
        error(string.format("unknown path field '%s'", tostring(key)), level or 3)
      end
    end

    local fill_type = normalize_path_fill_type(path.fill_type, (level or 3) + 1)
    if path.svg ~= nil then
      return raw_path_svg(self, path.svg, fill_type)
    end
    return raw_path(self, fill_type)
  end

  function canvas_mt:paint(paint)
    return compile_paint(self, paint, 3)
  end

  function canvas_mt:font(font)
    return compile_font(self, font, 3)
  end

  function canvas_mt:path(path)
    return compile_path(self, path, 3)
  end

  function canvas_mt:draw_rect(x, y, w, h, paint)
    return raw_draw_rect(self, x, y, w, h, compile_paint(self, paint, 3))
  end

  function canvas_mt:draw_path(path, paint)
    return raw_draw_path(self, compile_path(self, path, 3), compile_paint(self, paint, 3))
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

  function canvas_mt:clip_rect(path, op, anti_alias)
    return raw_clip_rect(
      self,
      compile_path(self, path, 3),
      normalize_clip_op(op, 3),
      anti_alias
    )
  end

  function canvas_mt:clip_path(path, op, anti_alias)
    return raw_clip_path(
      self,
      compile_path(self, path, 3),
      normalize_clip_op(op, 3),
      anti_alias
    )
  end

  function path_mt:set_fill_type(fill_type)
    return raw_path_set_fill_type(self, normalize_path_fill_type(fill_type, 3))
  end

  function path_mt:add_rect(x, y, w, h, direction)
    return raw_path_add_rect(self, x, y, w, h, normalize_path_direction(direction, 3))
  end

  function path_mt:add_oval(x, y, w, h, direction)
    return raw_path_add_oval(self, x, y, w, h, normalize_path_direction(direction, 3))
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
  luaL_getmetatable(L, LPath::MT);
  if (lua_pcall(L, 2, 0, 0) != 0) {
    lua_error(L);
  }
}

void LCanvas::RegisterBindings(lua_State *L) {
  lua::NewType<LPaint>(L);
  lua_pop(L, 1);
  lua::NewType<LFont>(L);
  lua_pop(L, 1);
  if (lua::NewType<LPath>(L)) {
    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      path->sk.moveTo(SkFloatToScalar(x), SkFloatToScalar(y));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "move_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      path->sk.lineTo(SkFloatToScalar(x), SkFloatToScalar(y));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "line_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x1 = static_cast<float>(luaL_checknumber(L, 2));
      const float y1 = static_cast<float>(luaL_checknumber(L, 3));
      const float x2 = static_cast<float>(luaL_checknumber(L, 4));
      const float y2 = static_cast<float>(luaL_checknumber(L, 5));
      path->sk.quadTo(SkFloatToScalar(x1),
                      SkFloatToScalar(y1),
                      SkFloatToScalar(x2),
                      SkFloatToScalar(y2));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "quad_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x1 = static_cast<float>(luaL_checknumber(L, 2));
      const float y1 = static_cast<float>(luaL_checknumber(L, 3));
      const float x2 = static_cast<float>(luaL_checknumber(L, 4));
      const float y2 = static_cast<float>(luaL_checknumber(L, 5));
      const float x3 = static_cast<float>(luaL_checknumber(L, 6));
      const float y3 = static_cast<float>(luaL_checknumber(L, 7));
      path->sk.cubicTo(SkFloatToScalar(x1),
                       SkFloatToScalar(y1),
                       SkFloatToScalar(x2),
                       SkFloatToScalar(y2),
                       SkFloatToScalar(x3),
                       SkFloatToScalar(y3));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "cubic_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x1 = static_cast<float>(luaL_checknumber(L, 2));
      const float y1 = static_cast<float>(luaL_checknumber(L, 3));
      const float x2 = static_cast<float>(luaL_checknumber(L, 4));
      const float y2 = static_cast<float>(luaL_checknumber(L, 5));
      const float weight = static_cast<float>(luaL_checknumber(L, 6));
      path->sk.conicTo(SkFloatToScalar(x1),
                       SkFloatToScalar(y1),
                       SkFloatToScalar(x2),
                       SkFloatToScalar(y2),
                       weight);
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "conic_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      path->sk.close();
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "close");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      path->sk.reset();
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "reset");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      path->sk.setFillType(CheckPathFillType(L, 2));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "_set_fill_type");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      const float w = static_cast<float>(luaL_checknumber(L, 4));
      const float h = static_cast<float>(luaL_checknumber(L, 5));
      SkPathDirection direction = SkPathDirection::kCW;
      if (!lua_isnoneornil(L, 6)) {
        direction = CheckPathDirection(L, 6);
      }
      path->sk.addRect(SkRect::MakeXYWH(SkFloatToScalar(x),
                                        SkFloatToScalar(y),
                                        SkFloatToScalar(w),
                                        SkFloatToScalar(h)),
                       direction);
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "_add_rect");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      const float w = static_cast<float>(luaL_checknumber(L, 4));
      const float h = static_cast<float>(luaL_checknumber(L, 5));
      SkPathDirection direction = SkPathDirection::kCW;
      if (!lua_isnoneornil(L, 6)) {
        direction = CheckPathDirection(L, 6);
      }
      path->sk.addOval(SkRect::MakeXYWH(SkFloatToScalar(x),
                                        SkFloatToScalar(y),
                                        SkFloatToScalar(w),
                                        SkFloatToScalar(h)),
                       direction);
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "_add_oval");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const bool relative =
          !lua_isnoneornil(L, 2) && (lua_toboolean(L, 2) != 0);
      const SkString svg = SkParsePath::ToSVGString(
          path->snapshot(),
          relative ? SkParsePath::PathEncoding::Relative
                   : SkParsePath::PathEncoding::Absolute);
      lua_pushlstring(L, svg.c_str(), svg.size());
      return 1;
    });
    lua_setfield(L, -2, "to_svg_string");
  }
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

    lua_newtable(L);

    lua_pushinteger(L, static_cast<lua_Integer>(SkPathFillType::kWinding));
    lua_setfield(L, -2, "winding");

    lua_pushinteger(L, static_cast<lua_Integer>(SkPathFillType::kEvenOdd));
    lua_setfield(L, -2, "even_odd");

    lua_pushinteger(L,
                    static_cast<lua_Integer>(SkPathFillType::kInverseWinding));
    lua_setfield(L, -2, "inverse_winding");

    lua_pushinteger(L,
                    static_cast<lua_Integer>(SkPathFillType::kInverseEvenOdd));
    lua_setfield(L, -2, "inverse_even_odd");

    lua_setfield(L, -2, "path_fill_type");

    lua_newtable(L);

    lua_pushinteger(L, static_cast<lua_Integer>(SkClipOp::kDifference));
    lua_setfield(L, -2, "difference");

    lua_pushinteger(L, static_cast<lua_Integer>(SkClipOp::kIntersect));
    lua_setfield(L, -2, "intersect");

    lua_setfield(L, -2, "clip_op");

    lua_newtable(L);

    lua_pushinteger(L, static_cast<lua_Integer>(SkPathDirection::kCW));
    lua_setfield(L, -2, "cw");

    lua_pushinteger(L, static_cast<lua_Integer>(SkPathDirection::kCCW));
    lua_setfield(L, -2, "ccw");

    lua_setfield(L, -2, "path_direction");

    lua_newtable(L);

    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kThin_Weight));
    lua_setfield(L, -2, "thin");
    lua_pushinteger(L,
                    static_cast<lua_Integer>(SkFontStyle::kExtraLight_Weight));
    lua_setfield(L, -2, "extra_light");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kLight_Weight));
    lua_setfield(L, -2, "light");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kNormal_Weight));
    lua_setfield(L, -2, "normal");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kMedium_Weight));
    lua_setfield(L, -2, "medium");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kSemiBold_Weight));
    lua_setfield(L, -2, "semi_bold");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kBold_Weight));
    lua_setfield(L, -2, "bold");
    lua_pushinteger(L,
                    static_cast<lua_Integer>(SkFontStyle::kExtraBold_Weight));
    lua_setfield(L, -2, "extra_bold");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kBlack_Weight));
    lua_setfield(L, -2, "black");
    lua_setfield(L, -2, "weight");

    lua_newtable(L);
    lua_pushinteger(
        L, static_cast<lua_Integer>(SkFontStyle::kUltraCondensed_Width));
    lua_setfield(L, -2, "ultra_condensed");
    lua_pushinteger(
        L, static_cast<lua_Integer>(SkFontStyle::kExtraCondensed_Width));
    lua_setfield(L, -2, "extra_condensed");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kCondensed_Width));
    lua_setfield(L, -2, "condensed");
    lua_pushinteger(
        L, static_cast<lua_Integer>(SkFontStyle::kSemiCondensed_Width));
    lua_setfield(L, -2, "semi_condensed");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kNormal_Width));
    lua_setfield(L, -2, "normal");
    lua_pushinteger(L,
                    static_cast<lua_Integer>(SkFontStyle::kSemiExpanded_Width));
    lua_setfield(L, -2, "semi_expanded");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kExpanded_Width));
    lua_setfield(L, -2, "expanded");
    lua_pushinteger(
        L, static_cast<lua_Integer>(SkFontStyle::kExtraExpanded_Width));
    lua_setfield(L, -2, "extra_expanded");
    lua_pushinteger(
        L, static_cast<lua_Integer>(SkFontStyle::kUltraExpanded_Width));
    lua_setfield(L, -2, "ultra_expanded");
    lua_setfield(L, -2, "width");

    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kUpright_Slant));
    lua_setfield(L, -2, "upright");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kItalic_Slant));
    lua_setfield(L, -2, "italic");
    lua_pushinteger(L, static_cast<lua_Integer>(SkFontStyle::kOblique_Slant));
    lua_setfield(L, -2, "oblique");
    lua_setfield(L, -2, "slant");

    lua_setfield(L, -2, "font_style");

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
      font->size = SkFloatToScalar(static_cast<float>(luaL_checknumber(L, 2)));
      if (!lua_isnoneornil(L, 3)) {
        const char *family = luaL_checkstring(L, 3);
        if (!family || !*family) {
          return luaL_error(L, "font: family must not be empty");
        }
        font->family_name = SkString(family);
        font->has_family = true;
      }
      const int weight = lua_isnoneornil(L, 4)
                             ? static_cast<int>(SkFontStyle::kNormal_Weight)
                             : static_cast<int>(luaL_checkinteger(L, 4));
      const int width = lua_isnoneornil(L, 5)
                            ? static_cast<int>(SkFontStyle::kNormal_Width)
                            : static_cast<int>(luaL_checkinteger(L, 5));
      const auto slant =
          lua_isnoneornil(L, 6)
              ? SkFontStyle::kUpright_Slant
              : static_cast<SkFontStyle::Slant>(luaL_checkinteger(L, 6));
      font->style = SkFontStyle(weight, width, slant);
      return 1;
    });
    lua_setfield(L, -2, "_font");

    lua::PushFunction(L, [](lua_State *L) {
      lua::Check<LCanvas>(L, 1);

      SkPathFillType fill_type = SkPathFillType::kWinding;
      if (!lua_isnoneornil(L, 2)) {
        fill_type = CheckPathFillType(L, 2);
      }

      lua::New<LPath>(L, fill_type);
      return 1;
    });
    lua_setfield(L, -2, "_path");

    lua::PushFunction(L, [](lua_State *L) {
      lua::Check<LCanvas>(L, 1);
      const char *svg = luaL_checkstring(L, 2);
      if (!svg || !*svg) {
        return luaL_error(L, "path: SVG path string must not be empty");
      }

      std::optional<SkPath> parsed = SkParsePath::FromSVGString(svg);
      if (!parsed.has_value()) {
        return luaL_error(L, "path: invalid SVG path string");
      }

      if (!lua_isnoneornil(L, 3)) {
        parsed->setFillType(CheckPathFillType(L, 3));
      }

      lua::New<LPath>(L, *parsed);
      return 1;
    });
    lua_setfield(L, -2, "_path_svg");

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
      LPath *path = lua::Check<LPath>(L, 2);

      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 3)) {
        paint = lua::Check<LPaint>(L, 3);
      }

      SkPaint default_paint;
      canvas->sk_->drawPath(path->snapshot(),
                            paint ? paint->sk : default_paint);
      return 0;
    });
    lua_setfield(L, -2, "_draw_path");

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
      const SkFont sk_font = ResolveFont(L, canvas, *font, "draw_text");
      canvas->sk_->drawSimpleText(text,
                                  text_len,
                                  SkTextEncoding::kUTF8,
                                  SkFloatToScalar(x),
                                  SkFloatToScalar(y),
                                  sk_font,
                                  paint ? paint->sk : default_paint);
      return 0;
    });
    lua_setfield(L, -2, "_draw_text");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      size_t text_len = 0;
      const char *text = luaL_checklstring(L, 2, &text_len);
      LFont *font = lua::Check<LFont>(L, 3);

      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 4)) {
        paint = lua::Check<LPaint>(L, 4);
      }

      SkRect bounds = SkRect::MakeEmpty();
      SkFontMetrics metrics;
      const SkFont sk_font = ResolveFont(L, canvas, *font, "measure_text");
      const SkScalar advance_width =
          sk_font.measureText(text,
                              text_len,
                              SkTextEncoding::kUTF8,
                              &bounds,
                              paint ? &paint->sk : nullptr);
      sk_font.getMetrics(&metrics);
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
      paragraph->sk->paint(canvas->sk_, SkFloatToScalar(x), SkFloatToScalar(y));
      return 0;
    });
    lua_setfield(L, -2, "_draw_paragraph");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      float x = static_cast<float>(luaL_checknumber(L, 2));
      float y = static_cast<float>(luaL_checknumber(L, 3));
      float w = static_cast<float>(luaL_checknumber(L, 4));
      float h = static_cast<float>(luaL_checknumber(L, 5));
      SkClipOp op = SkClipOp::kIntersect;
      if (!lua_isnoneornil(L, 6)) {
        op = CheckClipOp(L, 6);
      }

      bool anti_alias = false;
      if (!lua_isnoneornil(L, 7)) {
        luaL_checktype(L, 7, LUA_TBOOLEAN);
        anti_alias = lua_toboolean(L, 7) != 0;
      }

      canvas->sk_->clipRect(SkRect::MakeXYWH(SkFloatToScalar(x),
                                             SkFloatToScalar(y),
                                             SkFloatToScalar(w),
                                             SkFloatToScalar(h)),
                            op,
                            anti_alias);
      return 0;
    });
    lua_setfield(L, -2, "_clip_rect");

    lua::PushFunction(L, [](lua_State *L) {
      LCanvas *canvas = lua::Check<LCanvas>(L, 1);
      LPath *path = lua::Check<LPath>(L, 2);
      SkClipOp op = SkClipOp::kIntersect;
      if (!lua_isnoneornil(L, 3)) {
        op = CheckClipOp(L, 3);
      }

      bool anti_alias = false;
      if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TBOOLEAN);
        anti_alias = lua_toboolean(L, 4) != 0;
      }

      canvas->sk_->clipPath(path->snapshot(), op, anti_alias);
      return 0;
    });
    lua_setfield(L, -2, "_clip_path");

    RegisterLuaHelpers(L);
  }
  lua_pop(L, 1);
}

} // namespace luna
