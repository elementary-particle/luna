#ifndef LUNA_SKIA_ENUMS_H
#define LUNA_SKIA_ENUMS_H

#include <cstdint>

struct lua_State;

namespace luna::backend {

enum class PaintStyle : uint32_t {
  kFill,
  kStroke,
};

enum class ImageSamplingMode : uint32_t {
  kNearest = 0,
  kLinear = 1,
  kCubic = 2,
};

} // namespace luna::backend

#endif