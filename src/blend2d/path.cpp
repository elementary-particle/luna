#include "blend2d/canvas.h"

namespace luna::backend::blend2d {

void Path::SetFillType(BLFillRule fill_type) { fill_rule = fill_type; }

void Path::MoveTo(double x, double y) { bl.move_to(x, y); }

void Path::LineTo(double x, double y) { bl.line_to(x, y); }

void Path::QuadTo(double x1, double y1, double x2, double y2) {
  bl.quad_to(x1, y1, x2, y2);
}

void Path::SmoothQuadTo(double x2, double y2) { bl.smooth_quad_to(x2, y2); }

void Path::CubicTo(
    double x1, double y1, double x2, double y2, double x3, double y3) {
  bl.cubic_to(x1, y1, x2, y2, x3, y3);
}

void Path::SmoothCubicTo(double x2, double y2, double x3, double y3) {
  bl.smooth_cubic_to(x2, y2, x3, y3);
}

void Path::ArcTo(double rx, double ry, double x_axis_rotation,
    bool large_arc_flag, bool sweep_flag, double x1, double y1) {
  bl.elliptic_arc_to(
      rx, ry, x_axis_rotation, large_arc_flag, sweep_flag, x1, y1);
}

void Path::Close() { bl.close(); }

void Path::Reset() { bl.reset(); }

std::optional<Path> Path::FromSvgString(const char * /*svg*/) {
  return std::nullopt;
}

std::string Path::ToSvgString(bool /*relative*/) const { return {}; }

} // namespace luna::backend::blend2d
