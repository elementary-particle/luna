#include "blend2d/canvas.h"

#include <array>
#include <utility>

#include "lua_util.hpp"

namespace luna::backend::blend2d {

namespace {

static constexpr std::array PaintStyles{
    std::make_pair("fill", static_cast<uint32_t>(PaintStyle::kFill)),
    std::make_pair("stroke", static_cast<uint32_t>(PaintStyle::kStroke)),
};

static constexpr std::array PathFillTypes{
    std::make_pair("winding", static_cast<uint32_t>(BL_FILL_RULE_NON_ZERO)),
    std::make_pair("even_odd", static_cast<uint32_t>(BL_FILL_RULE_EVEN_ODD)),
};

static constexpr std::array TileModes{
    std::make_pair("clamp", static_cast<uint32_t>(BL_EXTEND_MODE_PAD)),
    std::make_pair("repeat", static_cast<uint32_t>(BL_EXTEND_MODE_REPEAT)),
    std::make_pair("mirror", static_cast<uint32_t>(BL_EXTEND_MODE_REFLECT)),
    std::make_pair("decal", static_cast<uint32_t>(BL_EXTEND_MODE_PAD_X_PAD_Y)),
};

static constexpr std::array ImageSamplingModes{
    std::make_pair("nearest", static_cast<uint32_t>(ImageSamplingMode::kNearest)),
    std::make_pair("linear", static_cast<uint32_t>(ImageSamplingMode::kLinear)),
    std::make_pair("cubic", static_cast<uint32_t>(ImageSamplingMode::kCubic)),
};

static constexpr std::array BlendModes{
    std::make_pair("clear", static_cast<uint32_t>(BL_COMP_OP_CLEAR)),
    std::make_pair("src", static_cast<uint32_t>(BL_COMP_OP_SRC_COPY)),
    std::make_pair("dst", static_cast<uint32_t>(BL_COMP_OP_DST_COPY)),
    std::make_pair("src_over", static_cast<uint32_t>(BL_COMP_OP_SRC_OVER)),
    std::make_pair("dst_over", static_cast<uint32_t>(BL_COMP_OP_DST_OVER)),
    std::make_pair("src_in", static_cast<uint32_t>(BL_COMP_OP_SRC_IN)),
    std::make_pair("dst_in", static_cast<uint32_t>(BL_COMP_OP_DST_IN)),
    std::make_pair("src_out", static_cast<uint32_t>(BL_COMP_OP_SRC_OUT)),
    std::make_pair("dst_out", static_cast<uint32_t>(BL_COMP_OP_DST_OUT)),
    std::make_pair("src_atop", static_cast<uint32_t>(BL_COMP_OP_SRC_ATOP)),
    std::make_pair("dst_atop", static_cast<uint32_t>(BL_COMP_OP_DST_ATOP)),
    std::make_pair("xor", static_cast<uint32_t>(BL_COMP_OP_XOR)),
    std::make_pair("plus", static_cast<uint32_t>(BL_COMP_OP_PLUS)),
    std::make_pair("modulate", static_cast<uint32_t>(BL_COMP_OP_MODULATE)),
    std::make_pair("screen", static_cast<uint32_t>(BL_COMP_OP_SCREEN)),
    std::make_pair("overlay", static_cast<uint32_t>(BL_COMP_OP_OVERLAY)),
    std::make_pair("darken", static_cast<uint32_t>(BL_COMP_OP_DARKEN)),
    std::make_pair("lighten", static_cast<uint32_t>(BL_COMP_OP_LIGHTEN)),
    std::make_pair("color_dodge", static_cast<uint32_t>(BL_COMP_OP_COLOR_DODGE)),
    std::make_pair("color_burn", static_cast<uint32_t>(BL_COMP_OP_COLOR_BURN)),
    std::make_pair("hard_light", static_cast<uint32_t>(BL_COMP_OP_HARD_LIGHT)),
    std::make_pair("soft_light", static_cast<uint32_t>(BL_COMP_OP_SOFT_LIGHT)),
    std::make_pair("difference", static_cast<uint32_t>(BL_COMP_OP_DIFFERENCE)),
    std::make_pair("exclusion", static_cast<uint32_t>(BL_COMP_OP_EXCLUSION)),
    std::make_pair("multiply", static_cast<uint32_t>(BL_COMP_OP_MULTIPLY)),
};

static constexpr std::array TextAligns{
    std::make_pair("left", static_cast<uint32_t>(TextAlign::kLeft)),
    std::make_pair("center", static_cast<uint32_t>(TextAlign::kCenter)),
    std::make_pair("right", static_cast<uint32_t>(TextAlign::kRight)),
};

static constexpr std::array FontStyleWeights{
    std::make_pair("thin", static_cast<uint32_t>(BL_FONT_WEIGHT_THIN)),
    std::make_pair("extra_light", static_cast<uint32_t>(BL_FONT_WEIGHT_EXTRA_LIGHT)),
    std::make_pair("light", static_cast<uint32_t>(BL_FONT_WEIGHT_LIGHT)),
    std::make_pair("normal", static_cast<uint32_t>(BL_FONT_WEIGHT_NORMAL)),
    std::make_pair("medium", static_cast<uint32_t>(BL_FONT_WEIGHT_MEDIUM)),
    std::make_pair("semi_bold", static_cast<uint32_t>(BL_FONT_WEIGHT_SEMI_BOLD)),
    std::make_pair("bold", static_cast<uint32_t>(BL_FONT_WEIGHT_BOLD)),
    std::make_pair("extra_bold", static_cast<uint32_t>(BL_FONT_WEIGHT_EXTRA_BOLD)),
    std::make_pair("black", static_cast<uint32_t>(BL_FONT_WEIGHT_BLACK)),
};

static constexpr std::array FontStyleWidths{
    std::make_pair("ultra_condensed", static_cast<uint32_t>(BL_FONT_STRETCH_ULTRA_CONDENSED)),
    std::make_pair("extra_condensed", static_cast<uint32_t>(BL_FONT_STRETCH_EXTRA_CONDENSED)),
    std::make_pair("condensed", static_cast<uint32_t>(BL_FONT_STRETCH_CONDENSED)),
    std::make_pair("semi_condensed", static_cast<uint32_t>(BL_FONT_STRETCH_SEMI_CONDENSED)),
    std::make_pair("normal", static_cast<uint32_t>(BL_FONT_STRETCH_NORMAL)),
    std::make_pair("semi_expanded", static_cast<uint32_t>(BL_FONT_STRETCH_SEMI_EXPANDED)),
    std::make_pair("expanded", static_cast<uint32_t>(BL_FONT_STRETCH_EXPANDED)),
    std::make_pair("extra_expanded", static_cast<uint32_t>(BL_FONT_STRETCH_EXTRA_EXPANDED)),
    std::make_pair("ultra_expanded", static_cast<uint32_t>(BL_FONT_STRETCH_ULTRA_EXPANDED)),
};

static constexpr std::array FontStyleSlants{
    std::make_pair("upright", static_cast<uint32_t>(BL_FONT_STYLE_NORMAL)),
    std::make_pair("italic", static_cast<uint32_t>(BL_FONT_STYLE_ITALIC)),
    std::make_pair("oblique", static_cast<uint32_t>(BL_FONT_STYLE_OBLIQUE)),
};

template <typename T, size_t N>
void SetEnum(lua_State *L,
    const std::array<std::pair<const char *, T>, N> &pairs,
    const char *enum_name) {
  lua_newtable(L);
  for (auto [k, v] : pairs) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    lua_setfield(L, -2, k);
  }
  lua_setfield(L, -2, enum_name);
}

} // namespace

void SetEnums(lua_State *L) {
  SetEnum(L, PaintStyles, "paint_style");
  SetEnum(L, PathFillTypes, "path_fill_type");
  SetEnum(L, TileModes, "tile_mode");
  SetEnum(L, ImageSamplingModes, "sampling");
  SetEnum(L, BlendModes, "blend_mode");
  SetEnum(L, TextAligns, "text_align");

  lua_newtable(L);
  SetEnum(L, FontStyleWeights, "weight");
  SetEnum(L, FontStyleWidths, "width");
  SetEnum(L, FontStyleSlants, "slant");
  lua_setfield(L, -2, "font_style");
}

} // namespace luna::backend::blend2d
