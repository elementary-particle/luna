#include "backend/enums.h"

#include <array>
#include <utility>

#include <skia/core/SkBlendMode.h>
#include <skia/core/SkClipOp.h>
#include <skia/core/SkFontStyle.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkPathTypes.h>
#include <skia/core/SkSamplingOptions.h>
#include <skia/core/SkTileMode.h>
#include <skia/modules/skparagraph/include/DartTypes.h>

#include "lua_util.hpp"

namespace luna::backend::skia {

namespace {

using ::skia::textlayout::TextAlign;

static constexpr std::array PaintStyles{
    std::make_pair("fill", SkPaint::kFill_Style),
    std::make_pair("stroke", SkPaint::kStroke_Style),
};

static constexpr std::array PathFillTypes{
    std::make_pair("winding", SkPathFillType::kWinding),
    std::make_pair("even_odd", SkPathFillType::kEvenOdd),
};

static constexpr std::array TileModes{
    std::make_pair("clamp", SkTileMode::kClamp),
    std::make_pair("repeat", SkTileMode::kRepeat),
    std::make_pair("mirror", SkTileMode::kMirror),
    std::make_pair("decal", SkTileMode::kDecal),
};

static constexpr std::array ImageSamplingModes{
    std::make_pair("nearest", ImageSamplingMode::kNearest),
    std::make_pair("linear", ImageSamplingMode::kLinear),
    std::make_pair("cubic", ImageSamplingMode::kCubic),
};

static constexpr std::array BlendModes{
    std::make_pair("clear", SkBlendMode::kClear),
    std::make_pair("src", SkBlendMode::kSrc),
    std::make_pair("dst", SkBlendMode::kDst),
    std::make_pair("src_over", SkBlendMode::kSrcOver),
    std::make_pair("dst_over", SkBlendMode::kDstOver),
    std::make_pair("src_in", SkBlendMode::kSrcIn),
    std::make_pair("dst_in", SkBlendMode::kDstIn),
    std::make_pair("src_out", SkBlendMode::kSrcOut),
    std::make_pair("dst_out", SkBlendMode::kDstOut),
    std::make_pair("src_atop", SkBlendMode::kSrcATop),
    std::make_pair("dst_atop", SkBlendMode::kDstATop),
    std::make_pair("xor", SkBlendMode::kXor),
    std::make_pair("plus", SkBlendMode::kPlus),
    std::make_pair("modulate", SkBlendMode::kModulate),
    std::make_pair("screen", SkBlendMode::kScreen),
    std::make_pair("overlay", SkBlendMode::kOverlay),
    std::make_pair("darken", SkBlendMode::kDarken),
    std::make_pair("lighten", SkBlendMode::kLighten),
    std::make_pair("color_dodge", SkBlendMode::kColorDodge),
    std::make_pair("color_burn", SkBlendMode::kColorBurn),
    std::make_pair("hard_light", SkBlendMode::kHardLight),
    std::make_pair("soft_light", SkBlendMode::kSoftLight),
    std::make_pair("difference", SkBlendMode::kDifference),
    std::make_pair("exclusion", SkBlendMode::kExclusion),
    std::make_pair("multiply", SkBlendMode::kMultiply),
};

static constexpr std::array TextAligns{
    std::make_pair("left", TextAlign::kLeft),
    std::make_pair("center", TextAlign::kCenter),
    std::make_pair("right", TextAlign::kRight),
};

static constexpr std::array PathDirections{
    std::make_pair("cw", SkPathDirection::kCW),
    std::make_pair("ccw", SkPathDirection::kCCW),
};

static constexpr std::array FontStyleWeights{
    std::make_pair("thin", SkFontStyle::kThin_Weight),
    std::make_pair("extra_light", SkFontStyle::kExtraLight_Weight),
    std::make_pair("light", SkFontStyle::kLight_Weight),
    std::make_pair("normal", SkFontStyle::kNormal_Weight),
    std::make_pair("medium", SkFontStyle::kMedium_Weight),
    std::make_pair("semi_bold", SkFontStyle::kSemiBold_Weight),
    std::make_pair("bold", SkFontStyle::kBold_Weight),
    std::make_pair("extra_bold", SkFontStyle::kExtraBold_Weight),
    std::make_pair("black", SkFontStyle::kBlack_Weight),
};

static constexpr std::array FontStyleWidths{
    std::make_pair("ultra_condensed", SkFontStyle::kUltraCondensed_Width),
    std::make_pair("extra_condensed", SkFontStyle::kExtraCondensed_Width),
    std::make_pair("condensed", SkFontStyle::kCondensed_Width),
    std::make_pair("semi_condensed", SkFontStyle::kSemiCondensed_Width),
    std::make_pair("normal", SkFontStyle::kNormal_Width),
    std::make_pair("semi_expanded", SkFontStyle::kSemiExpanded_Width),
    std::make_pair("expanded", SkFontStyle::kExpanded_Width),
    std::make_pair("extra_expanded", SkFontStyle::kExtraExpanded_Width),
    std::make_pair("ultra_expanded", SkFontStyle::kUltraExpanded_Width),
};

static constexpr std::array FontStyleSlants{
    std::make_pair("upright", SkFontStyle::kUpright_Slant),
    std::make_pair("italic", SkFontStyle::kItalic_Slant),
    std::make_pair("oblique", SkFontStyle::kOblique_Slant),
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
  SetEnum(L, PathDirections, "path_direction");

  lua_newtable(L);
  SetEnum(L, FontStyleWeights, "weight");
  SetEnum(L, FontStyleWidths, "width");
  SetEnum(L, FontStyleSlants, "slant");
  lua_setfield(L, -2, "font_style");
}

} // namespace luna::backend::skia
