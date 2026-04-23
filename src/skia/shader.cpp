#include "canvas.h"

#include <vector>

#include <skia/core/SkColor.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPoint.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkSamplingOptions.h>
#include <skia/core/SkShader.h>
#include <skia/effects/SkGradient.h>

namespace luna::backend::skia {

namespace {

std::vector<SkColor4f> ToColorArray(std::span<const uint32_t> colors) {
  std::vector<SkColor4f> out;
  out.reserve(colors.size());
  for (uint32_t color : colors) {
    out.push_back(SkColor4f::FromColor(static_cast<SkColor>(color)));
  }
  return out;
}

SkSamplingOptions ResolveSampling(ImageSamplingMode sampling) {
  switch (sampling) {
  case ImageSamplingMode::kLinear:
    return SkSamplingOptions(SkFilterMode::kLinear);
  case ImageSamplingMode::kCubic:
    return SkSamplingOptions(SkCubicResampler::Mitchell());
  case ImageSamplingMode::kNearest:
  default:
    return SkSamplingOptions(SkFilterMode::kNearest);
  }
}

} // namespace

void Shader::MakeSolidColor(uint32_t color) {
  sk = SkShaders::Color(static_cast<SkColor>(color));
}

bool Shader::MakeLinearGradient(double x0, double y0, double x1, double y1,
    std::span<const uint32_t> colors, std::span<const float> positions,
    SkTileMode tile_mode, std::string *error) {
  const std::vector<SkColor4f> gradient_colors = ToColorArray(colors);
  const float *positions_ptr = positions.empty() ? nullptr : positions.data();
  const SkGradient grad(
      SkGradient::Colors(gradient_colors,
          SkSpan<const float>(positions_ptr, positions.size()), tile_mode),
      {});
  const SkPoint points[2] = {
      SkPoint::Make(SkFloatToScalar(static_cast<float>(x0)),
          SkFloatToScalar(static_cast<float>(y0))),
      SkPoint::Make(SkFloatToScalar(static_cast<float>(x1)),
          SkFloatToScalar(static_cast<float>(y1))),
  };
  sk = SkShaders::LinearGradient(points, grad);
  if (!sk) {
    *error = "shader.linear_gradient: failed to create shader";
    return false;
  }
  return true;
}

bool Shader::MakeRadialGradient(double cx, double cy, double radius,
    std::span<const uint32_t> colors, std::span<const float> positions,
    SkTileMode tile_mode, std::string *error) {
  const std::vector<SkColor4f> gradient_colors = ToColorArray(colors);
  const float *positions_ptr = positions.empty() ? nullptr : positions.data();
  const SkGradient grad(
      SkGradient::Colors(gradient_colors,
          SkSpan<const float>(positions_ptr, positions.size()), tile_mode),
      {});
  sk = SkShaders::RadialGradient(
      SkPoint::Make(SkFloatToScalar(static_cast<float>(cx)),
          SkFloatToScalar(static_cast<float>(cy))),
      static_cast<float>(radius), grad);
  if (!sk) {
    *error = "shader.radial_gradient: failed to create shader";
    return false;
  }
  return true;
}

void Shader::MakeImage(Image *image, ImageSamplingMode sampling,
    SkTileMode tile_mode, std::string *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (image == nullptr || !image->sk) {
    if (error != nullptr) {
      *error = "shader.image: image is empty";
    }
    sk.reset();
    return;
  }
  sk = image->sk->makeShader(tile_mode, tile_mode, ResolveSampling(sampling));
  if (!sk && error != nullptr) {
    *error = "shader.image: failed to create shader";
  }
}

} // namespace luna::backend::skia
