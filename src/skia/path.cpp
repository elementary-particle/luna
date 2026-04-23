#include "skia/canvas.h"

#include <optional>
#include <string>

#include <skia/core/SkRRect.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTypes.h>
#include <skia/utils/SkParsePath.h>

namespace luna::backend::skia {

void Path::SetFillType(SkPathFillType fill_type) { sk.setFillType(fill_type); }

void Path::MoveTo(double x, double y) {
  sk.moveTo(SkDoubleToScalar(x), SkDoubleToScalar(y));
}

void Path::LineTo(double x, double y) {
  sk.lineTo(SkDoubleToScalar(x), SkDoubleToScalar(y));
}

void Path::QuadTo(double x1, double y1, double x2, double y2) {
  sk.quadTo(SkDoubleToScalar(x1), SkDoubleToScalar(y1), SkDoubleToScalar(x2),
      SkDoubleToScalar(y2));
}

void Path::SmoothQuadTo(double x2, double y2) {
  // TODO
}

void Path::CubicTo(
    double x1, double y1, double x2, double y2, double x3, double y3) {
  sk.cubicTo(SkDoubleToScalar(x1), SkDoubleToScalar(y1), SkDoubleToScalar(x2),
      SkDoubleToScalar(y2), SkDoubleToScalar(x3), SkDoubleToScalar(y3));
}

void Path::SmoothCubicTo(double x2, double y2, double x3, double y3) {
  // TODO
}

void Path::ArcTo(double rx, double ry, double x_axis_rotation,
    bool large_arc_flag, bool sweep_flag, double x1, double y1) {
  sk.arcTo(SkPoint(SkDoubleToScalar(rx), SkDoubleToScalar(ry)),
      SkDoubleToScalar(x_axis_rotation),
      large_arc_flag ? SkPathBuilder::kLarge_ArcSize
                     : SkPathBuilder::kSmall_ArcSize,
      sweep_flag ? SkPathDirection::kCW : SkPathDirection::kCCW,
      SkPoint(SkDoubleToScalar(x1), SkDoubleToScalar(y1)));
}

void Path::Close() { sk.close(); }

void Path::Reset() { sk.reset(); }

std::optional<Path> Path::FromSvgString(const char *svg) {
  std::optional<SkPath> parsed = SkParsePath::FromSVGString(svg);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  return Path(*parsed);
}

std::string Path::ToSvgString(bool relative) const {
  const SkString svg = SkParsePath::ToSVGString(snapshot(),
      relative ? SkParsePath::PathEncoding::Relative
               : SkParsePath::PathEncoding::Absolute);
  return std::string(svg.c_str(), svg.size());
}

} // namespace luna::backend::skia
