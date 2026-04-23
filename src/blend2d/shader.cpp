#include "blend2d/canvas.h"

#include <vector>

namespace luna::backend::blend2d {

namespace {

BLPatternQuality ResolvePatternQuality(ImageSamplingMode sampling) {
  switch (sampling) {
  case ImageSamplingMode::kNearest:
    return BL_PATTERN_QUALITY_NEAREST;
  case ImageSamplingMode::kCubic:
  case ImageSamplingMode::kLinear:
  default:
    return BL_PATTERN_QUALITY_BILINEAR;
  }
}

} // namespace

void Shader::MakeSolidColor(uint32_t color) {
  image_sampling = ImageSamplingMode::kLinear;
  *this = BLRgba32(color);
}

bool Shader::MakeLinearGradient(double x0, double y0, double x1, double y1,
    std::span<const uint32_t> colors, std::span<const float> positions,
    BLExtendMode tile_mode, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }

  BLGradient gradient(BLLinearGradientValues{x0, y0, x1, y1}, tile_mode);
  for (size_t i = 0; i < colors.size(); ++i) {
    const double offset =
        positions.empty() ? double(i) / double(colors.size() - 1) : positions[i];
    if (gradient.add_stop(offset, BLRgba32(colors[i])) != BL_SUCCESS) {
      if (error != nullptr) {
        *error = "shader.linear_gradient: failed to add stop";
      }
      return false;
    }
  }

  image_sampling = ImageSamplingMode::kLinear;
  *this = std::move(gradient);
  return true;
}

bool Shader::MakeRadialGradient(double cx, double cy, double radius,
    std::span<const uint32_t> colors, std::span<const float> positions,
    BLExtendMode tile_mode, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }

  BLGradient gradient(
      BLRadialGradientValues{cx, cy, cx, cy, radius, 0.0}, tile_mode);
  for (size_t i = 0; i < colors.size(); ++i) {
    const double offset =
        positions.empty() ? double(i) / double(colors.size() - 1) : positions[i];
    if (gradient.add_stop(offset, BLRgba32(colors[i])) != BL_SUCCESS) {
      if (error != nullptr) {
        *error = "shader.radial_gradient: failed to add stop";
      }
      return false;
    }
  }

  image_sampling = ImageSamplingMode::kLinear;
  *this = std::move(gradient);
  return true;
}

void Shader::MakeImage(Image *image, ImageSamplingMode sampling,
    BLExtendMode tile_mode, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (image == nullptr || image->image.is_empty()) {
    if (error != nullptr) {
      *error = "shader.image: image is empty";
    }
    *this = BLRgba32(0);
    return;
  }
  BLPattern pattern(image->image, tile_mode);
  image_sampling = sampling == ImageSamplingMode::kCubic
      ? ImageSamplingMode::kLinear
      : sampling;
  *this = std::move(pattern);
}

} // namespace luna::backend::blend2d
