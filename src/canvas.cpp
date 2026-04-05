#include "canvas_bindings.h"

#include <cstring>

#include <skia/core/SkImage.h>
#include <skia/core/SkBlendMode.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkRRect.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkSamplingOptions.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTileMode.h>
#include <skia/core/SkTypeface.h>
#include <skia/core/SkTypes.h>
#include <skia/modules/skparagraph/include/Metrics.h>

namespace luna {

namespace {

SkPaint::Style CheckPaintStyle(lua_State *L, int idx) {
  return static_cast<SkPaint::Style>(luaL_checkinteger(L, idx));
}

SkBlendMode CheckBlendMode(lua_State *L, int idx) {
  return static_cast<SkBlendMode>(luaL_checkinteger(L, idx));
}

SkClipOp CheckClipOp(lua_State *L, int idx) {
  return static_cast<SkClipOp>(luaL_checkinteger(L, idx));
}

void PushTextMetricsTable(lua_State *L, SkScalar advance_width,
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

namespace canvas {

void RegisterCanvasLuaHelpers(lua_State *L) {
  static constexpr char CANVAS_HELPERS[] = R"(
return function(canvas_mt)
  local raw_paint = assert(canvas_mt._paint)
  local raw_font = assert(canvas_mt._font)
  local raw_draw_rect = assert(canvas_mt._draw_rect)
  local raw_draw_round_rect = assert(canvas_mt._draw_round_rect)
  local raw_draw_image_rect = assert(canvas_mt._draw_image_rect)
  local raw_draw_text = assert(canvas_mt._draw_text)
  local raw_measure_text = assert(canvas_mt._measure_text)
  local raw_clip_rect = assert(canvas_mt._clip_rect)
  local raw_clip_round_rect = assert(canvas_mt._clip_round_rect)
  local raw_save_layer = assert(canvas_mt._save_layer)

  local constants = assert(canvas_mt._constants)

  local paint_style_ = {
    fill = constants.paint_style.fill,
    stroke = constants.paint_style.stroke,
    stroke_and_fill = constants.paint_style.stroke_and_fill,
    fill_and_stroke = constants.paint_style.stroke_and_fill,
  }

  local clip_op_ = {
    difference = constants.clip_op.difference,
    intersect = constants.clip_op.intersect,
  }

  local blend_mode_ = {
    clear = constants.blend_mode.clear,
    src = constants.blend_mode.src,
    dst = constants.blend_mode.dst,
    src_over = constants.blend_mode.src_over,
    dst_over = constants.blend_mode.dst_over,
    src_in = constants.blend_mode.src_in,
    dst_in = constants.blend_mode.dst_in,
    src_out = constants.blend_mode.src_out,
    dst_out = constants.blend_mode.dst_out,
    src_atop = constants.blend_mode.src_atop,
    dst_atop = constants.blend_mode.dst_atop,
    xor = constants.blend_mode.xor,
    plus = constants.blend_mode.plus,
    modulate = constants.blend_mode.modulate,
    screen = constants.blend_mode.screen,
    overlay = constants.blend_mode.overlay,
    darken = constants.blend_mode.darken,
    lighten = constants.blend_mode.lighten,
    color_dodge = constants.blend_mode.color_dodge,
    color_burn = constants.blend_mode.color_burn,
    hard_light = constants.blend_mode.hard_light,
    soft_light = constants.blend_mode.soft_light,
    difference = constants.blend_mode.difference,
    exclusion = constants.blend_mode.exclusion,
    multiply = constants.blend_mode.multiply,
    hue = constants.blend_mode.hue,
    saturation = constants.blend_mode.saturation,
    color = constants.blend_mode.color,
    luminosity = constants.blend_mode.luminosity,
  }

  local allowed_paint_keys = {
    anti_alias = true,
    blend_mode = true,
    color = true,
    shader = true,
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

  local function normalize_blend_mode(mode, level)
    if mode == nil then
      return nil
    end
    if type(mode) == "number" then
      return mode
    end
    if type(mode) ~= "string" then
      error("paint.blend_mode must be a number or string", level or 3)
    end

    local mapped = blend_mode_[mode]
    if mapped == nil then
      error(string.format("invalid paint.blend_mode '%s'", mode), level or 3)
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
      paint.stroke_width,
      normalize_blend_mode(paint.blend_mode, (level or 3) + 1),
      self:shader(paint.shader)
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

  function canvas_mt:paint(paint)
    return compile_paint(self, paint, 3)
  end

  function canvas_mt:font(font)
    return compile_font(self, font, 3)
  end

  function canvas_mt:draw_rect(x, y, w, h, paint)
    return raw_draw_rect(self, x, y, w, h, compile_paint(self, paint, 3))
  end

  function canvas_mt:draw_rrect(x, y, w, h, rx, ry, paint)
    return raw_draw_round_rect(self, x, y, w, h, rx, ry, compile_paint(self, paint, 3))
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

  function canvas_mt:clip_rect(x, y, w, h, op, anti_alias)
    return raw_clip_rect(self, x, y, w, h, normalize_clip_op(op, 3), anti_alias)
  end

  function canvas_mt:clip_rrect(x, y, w, h, rx, ry, op, anti_alias)
    return raw_clip_round_rect(self, x, y, w, h, rx, ry, normalize_clip_op(op, 3), anti_alias)
  end

  function canvas_mt:save_layer(paint)
    return raw_save_layer(self, compile_paint(self, paint, 3))
  end
end
)";

  if (luaL_loadbuffer(L, CANVAS_HELPERS, std::strlen(CANVAS_HELPERS),
                      "canvas_helpers") != 0) {
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

} // namespace canvas

void LCanvas::RegisterBindings(lua_State *L) {
  lua::NewType<LPaint>(L);
  lua_pop(L, 1);
  lua::NewType<LFont>(L);
  lua_pop(L, 1);
  canvas::RegisterPathBindings(L);
  canvas::RegisterParagraphBindings(L);
  canvas::RegisterShaderBindings(L);

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
      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 2)) {
        paint = lua::Check<LPaint>(L, 2);
      }
      canvas->sk_->saveLayer(nullptr, paint ? &paint->sk : nullptr);
      return 0;
    });
    lua_setfield(L, -2, "_save_layer");

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
    lua_pushinteger(
        L, static_cast<lua_Integer>(skia::textlayout::TextAlign::kLeft));
    lua_setfield(L, -2, "left");
    lua_pushinteger(
        L, static_cast<lua_Integer>(skia::textlayout::TextAlign::kCenter));
    lua_setfield(L, -2, "center");
    lua_pushinteger(
        L, static_cast<lua_Integer>(skia::textlayout::TextAlign::kRight));
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
    lua_pushinteger(L, static_cast<lua_Integer>(SkTileMode::kClamp));
    lua_setfield(L, -2, "clamp");
    lua_pushinteger(L, static_cast<lua_Integer>(SkTileMode::kRepeat));
    lua_setfield(L, -2, "repeat");
    lua_pushinteger(L, static_cast<lua_Integer>(SkTileMode::kMirror));
    lua_setfield(L, -2, "mirror");
    lua_pushinteger(L, static_cast<lua_Integer>(SkTileMode::kDecal));
    lua_setfield(L, -2, "decal");
    lua_setfield(L, -2, "tile_mode");

    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kClear));
    lua_setfield(L, -2, "clear");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kSrc));
    lua_setfield(L, -2, "src");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kDst));
    lua_setfield(L, -2, "dst");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kSrcOver));
    lua_setfield(L, -2, "src_over");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kDstOver));
    lua_setfield(L, -2, "dst_over");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kSrcIn));
    lua_setfield(L, -2, "src_in");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kDstIn));
    lua_setfield(L, -2, "dst_in");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kSrcOut));
    lua_setfield(L, -2, "src_out");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kDstOut));
    lua_setfield(L, -2, "dst_out");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kSrcATop));
    lua_setfield(L, -2, "src_atop");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kDstATop));
    lua_setfield(L, -2, "dst_atop");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kXor));
    lua_setfield(L, -2, "xor");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kPlus));
    lua_setfield(L, -2, "plus");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kModulate));
    lua_setfield(L, -2, "modulate");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kScreen));
    lua_setfield(L, -2, "screen");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kOverlay));
    lua_setfield(L, -2, "overlay");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kDarken));
    lua_setfield(L, -2, "darken");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kLighten));
    lua_setfield(L, -2, "lighten");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kColorDodge));
    lua_setfield(L, -2, "color_dodge");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kColorBurn));
    lua_setfield(L, -2, "color_burn");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kHardLight));
    lua_setfield(L, -2, "hard_light");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kSoftLight));
    lua_setfield(L, -2, "soft_light");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kDifference));
    lua_setfield(L, -2, "difference");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kExclusion));
    lua_setfield(L, -2, "exclusion");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kMultiply));
    lua_setfield(L, -2, "multiply");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kHue));
    lua_setfield(L, -2, "hue");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kSaturation));
    lua_setfield(L, -2, "saturation");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kColor));
    lua_setfield(L, -2, "color");
    lua_pushinteger(L, static_cast<lua_Integer>(SkBlendMode::kLuminosity));
    lua_setfield(L, -2, "luminosity");
    lua_setfield(L, -2, "blend_mode");

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
    lua_pushinteger(
        L, static_cast<lua_Integer>(SkFontStyle::kSemiExpanded_Width));
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
      if (!lua_isnoneornil(L, 6)) {
        paint->sk.setBlendMode(CheckBlendMode(L, 6));
      }
      if (!lua_isnoneornil(L, 7)) {
        LShader *shader = lua::Check<LShader>(L, 7);
        paint->sk.setShader(shader->sk);
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
      float x = static_cast<float>(luaL_checknumber(L, 2));
      float y = static_cast<float>(luaL_checknumber(L, 3));
      float w = static_cast<float>(luaL_checknumber(L, 4));
      float h = static_cast<float>(luaL_checknumber(L, 5));
      float rx = static_cast<float>(luaL_checknumber(L, 6));
      float ry = static_cast<float>(luaL_checknumber(L, 7));

      LPaint *paint = nullptr;
      if (!lua_isnoneornil(L, 8)) {
        paint = lua::Check<LPaint>(L, 8);
      }

      SkPaint default_paint;
      canvas->sk_->drawRRect(
          SkRRect::MakeRectXY(SkRect::MakeXYWH(SkFloatToScalar(x),
                                               SkFloatToScalar(y),
                                               SkFloatToScalar(w),
                                               SkFloatToScalar(h)),
                              SkFloatToScalar(rx),
                              SkFloatToScalar(ry)),
          paint ? paint->sk : default_paint);
      return 0;
    });
    lua_setfield(L, -2, "_draw_round_rect");

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
      const SkFont sk_font =
          ResolveFont(L, canvas, *font, "draw_text");
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
      const SkFont sk_font =
          ResolveFont(L, canvas, *font, "measure_text");
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
      float x = static_cast<float>(luaL_checknumber(L, 2));
      float y = static_cast<float>(luaL_checknumber(L, 3));
      float w = static_cast<float>(luaL_checknumber(L, 4));
      float h = static_cast<float>(luaL_checknumber(L, 5));
      float rx = static_cast<float>(luaL_checknumber(L, 6));
      float ry = static_cast<float>(luaL_checknumber(L, 7));
      SkClipOp op = SkClipOp::kIntersect;
      if (!lua_isnoneornil(L, 8)) {
        op = CheckClipOp(L, 8);
      }

      bool anti_alias = false;
      if (!lua_isnoneornil(L, 9)) {
        luaL_checktype(L, 9, LUA_TBOOLEAN);
        anti_alias = lua_toboolean(L, 9) != 0;
      }

      canvas->sk_->clipRRect(
          SkRRect::MakeRectXY(SkRect::MakeXYWH(SkFloatToScalar(x),
                                               SkFloatToScalar(y),
                                               SkFloatToScalar(w),
                                               SkFloatToScalar(h)),
                              SkFloatToScalar(rx),
                              SkFloatToScalar(ry)),
          op,
          anti_alias);
      return 0;
    });
    lua_setfield(L, -2, "_clip_round_rect");

    canvas::RegisterPathCanvasMethods(L);
    canvas::RegisterParagraphCanvasMethods(L);
    canvas::RegisterShaderCanvasMethods(L);
    canvas::RegisterCanvasLuaHelpers(L);
    canvas::RegisterPathLuaHelpers(L);
    canvas::RegisterParagraphLuaHelpers(L);
    canvas::RegisterShaderLuaHelpers(L);
  }
  lua_pop(L, 1);
}

} // namespace luna
