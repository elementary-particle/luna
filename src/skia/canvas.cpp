#include "skia/canvas.h"

#include <skia/core/SkBlendMode.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkRRect.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkSamplingOptions.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTypeface.h>
#include <skia/core/SkTypes.h>
#include <skia/modules/skparagraph/include/Metrics.h>

#include "backend/enums.h"

namespace luna::backend::skia {

namespace {

bool ResolveFont(const Canvas &canvas, const Canvas::Font &font,
    const char *operation, SkFont *resolved, std::string *error) {
  resolved->setSize(font.size);

  if (font.family_name.size() == 0) {
    *error = std::string(operation) + "no font family specified";
    return false;
  }

  if (!canvas.font_mgr()) {
    *error = std::string(operation) + ": canvas has no font manager";
    return false;
  }

  sk_sp<SkTypeface> typeface =
      canvas.font_mgr()->matchFamilyStyle(font.family_name.c_str(), font.style);
  if (!typeface) {
    *error = std::string(operation) + ": failed to resolve family '" +
        font.family_name.c_str() + "'";
    return false;
  }

  resolved->setTypeface(std::move(typeface));
  return true;
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

bool HitTestPathWithCurrentTransform(
    SkCanvas *canvas, const SkPath &path,
    const SkMatrix &window_to_surface_matrix, double hit_x, double hit_y) {
  const std::optional<SkPath> transformed =
      path.tryMakeTransform(canvas->getTotalMatrix());
  if (!transformed.has_value()) {
    return false;
  }
  const SkPoint hit_point = window_to_surface_matrix.mapPoint(
      SkPoint::Make(SkDoubleToScalar(hit_x), SkDoubleToScalar(hit_y)));
  return transformed->contains(hit_point.fX, hit_point.fY);
}

} // namespace

void Canvas::Paint::SetShader(const Shader &shader) { sk.setShader(shader.sk); }

void Canvas::Paint::SetAlpha(double alpha) {
  sk.setAlphaf(static_cast<float>(alpha));
}

void Canvas::Paint::SetBlendMode(SkBlendMode blend_mode) {
  sk.setBlendMode(blend_mode);
}

void Canvas::Paint::SetFillStyle() { sk.setStyle(SkPaint::kFill_Style); }

void Canvas::Paint::SetStrokeStyle(double stroke_width) {
  sk.setStyle(SkPaint::kStroke_Style);
  sk.setStrokeWidth(SkDoubleToScalar(stroke_width));
}

void Canvas::Font::SetSize(double size_) { size = SkDoubleToScalar(size_); }

void Canvas::Font::SetFamily(const char *family) {
  family_name = SkString(family);
}

void Canvas::Font::SetStyle(int weight, int width, int slant) {
  style = SkFontStyle(weight, width, static_cast<SkFontStyle::Slant>(slant));
}

void Canvas::Clear(uint32_t color) {
  SkPaint paint;
  paint.setColor(static_cast<SkColor>(color));
  paint.setBlendMode(SkBlendMode::kSrc);
  sk_->drawPaint(paint);
}

void Canvas::Save() { sk_->save(); }

void Canvas::SaveLayer(const Paint *paint) {
  sk_->saveLayer(nullptr, paint ? &paint->sk : nullptr);
}

void Canvas::Restore() { sk_->restore(); }

void Canvas::Translate(double dx, double dy) {
  sk_->translate(SkDoubleToScalar(dx), SkDoubleToScalar(dy));
}

void Canvas::Scale(double sx, double sy) {
  sk_->scale(SkDoubleToScalar(sx), SkDoubleToScalar(sy));
}

void Canvas::Skew(double sx, double sy) {
  sk_->skew(SkDoubleToScalar(sx), SkDoubleToScalar(sy));
}

void Canvas::Rotate(double radians) {
  sk_->rotate(SkRadiansToDegrees(SkDoubleToScalar(radians)));
}

bool Canvas::Snapshot(Image *image, std::string *error) const {
  if (!surface_) {
    *error = "snapshot: canvas does not support snapshots";
    return false;
  }

  image->sk = surface_->makeImageSnapshot();
  if (!image->sk) {
    *error = "snapshot: failed to create image";
    return false;
  }
  return true;
}

bool Canvas::HitTestRect(
    double x, double y, double w, double h, double hit_x, double hit_y) {
  const SkPath rect = SkPath::Rect(SkRect::MakeXYWH(SkDoubleToScalar(x),
      SkDoubleToScalar(y), SkDoubleToScalar(w), SkDoubleToScalar(h)));
  return HitTestPathWithCurrentTransform(
      sk_, rect, window_to_surface_matrix_, hit_x, hit_y);
}

bool Canvas::HitTestPath(const Path &path, double hit_x, double hit_y) {
  return HitTestPathWithCurrentTransform(
      sk_, path.snapshot(), window_to_surface_matrix_, hit_x, hit_y);
}

void Canvas::DrawRect(
    double x, double y, double w, double h, const Paint *paint) {
  SkPaint default_paint;
  sk_->drawRect(SkRect::MakeXYWH(SkDoubleToScalar(x), SkDoubleToScalar(y),
                    SkDoubleToScalar(w), SkDoubleToScalar(h)),
      paint ? paint->sk : default_paint);
}

void Canvas::DrawRoundRect(double x, double y, double w, double h, double rx,
    double ry, const Paint *paint) {
  SkPaint default_paint;
  sk_->drawRRect(SkRRect::MakeRectXY(
                     SkRect::MakeXYWH(SkDoubleToScalar(x), SkDoubleToScalar(y),
                         SkDoubleToScalar(w), SkDoubleToScalar(h)),
                     SkDoubleToScalar(rx), SkDoubleToScalar(ry)),
      paint ? paint->sk : default_paint);
}

void Canvas::DrawPath(const Path &path, const Paint *paint) {
  SkPaint default_paint;
  sk_->drawPath(path.snapshot(), paint ? paint->sk : default_paint);
}

void Canvas::ClipRect(double x, double y, double w, double h) {
  sk_->clipRect(SkRect::MakeXYWH(SkDoubleToScalar(x), SkDoubleToScalar(y),
                    SkDoubleToScalar(w), SkDoubleToScalar(h)),
      SkClipOp::kIntersect, true);
}

void Canvas::ClipRoundRect(
    double x, double y, double w, double h, double rx, double ry) {
  sk_->clipRRect(SkRRect::MakeRectXY(
                     SkRect::MakeXYWH(SkDoubleToScalar(x), SkDoubleToScalar(y),
                         SkDoubleToScalar(w), SkDoubleToScalar(h)),
                     SkDoubleToScalar(rx), SkDoubleToScalar(ry)),
      SkClipOp::kIntersect, true);
}

void Canvas::ClipPath(const Path &path) {
  sk_->clipPath(path.snapshot(), SkClipOp::kIntersect, true);
}

bool Canvas::MeasureText(std::string_view text, const Font &font,
    const Paint *paint, TextMetrics *metrics, std::string *error) {
  SkRect bounds = SkRect::MakeEmpty();
  SkFontMetrics font_metrics;
  SkFont resolved_font;
  if (!ResolveFont(*this, font, "measure_text", &resolved_font, error)) {
    return false;
  }

  metrics->advance_width = resolved_font.measureText(text.data(), text.size(),
      SkTextEncoding::kUTF8, &bounds, paint ? &paint->sk : nullptr);
  resolved_font.getMetrics(&font_metrics);
  metrics->bounds_x = bounds.x();
  metrics->bounds_y = bounds.y();
  metrics->bounds_w = bounds.width();
  metrics->bounds_h = bounds.height();
  metrics->ascent = font_metrics.fAscent;
  metrics->descent = font_metrics.fDescent;
  metrics->leading = font_metrics.fLeading;
  metrics->line_height =
      font_metrics.fDescent - font_metrics.fAscent + font_metrics.fLeading;
  return true;
}

bool Canvas::DrawText(std::string_view text, double x, double y,
    const Font &font, const Paint *paint, std::string *error) {
  SkPaint default_paint;
  SkFont resolved_font;
  if (!ResolveFont(*this, font, "draw_text", &resolved_font, error)) {
    return false;
  }

  sk_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
      SkDoubleToScalar(x), SkDoubleToScalar(y), resolved_font,
      paint ? paint->sk : default_paint);
  return true;
}

Backend::FillType Backend::DefaultFillType() {
  return SkPathFillType::kWinding;
}

Backend::ImageSamplingMode Backend::DefaultImageSamplingMode() {
  return ImageSamplingMode::kLinear;
}

Backend::TileMode Backend::DefaultTileMode() { return SkTileMode::kClamp; }

Backend::TextAlign Backend::DefaultTextAlign() { return TextAlign::kLeft; }

int Backend::DefaultFontWeight() {
  return static_cast<int>(SkFontStyle::kNormal_Weight);
}

int Backend::DefaultFontWidth() {
  return static_cast<int>(SkFontStyle::kNormal_Width);
}

int Backend::DefaultFontSlant() {
  return static_cast<int>(SkFontStyle::kUpright_Slant);
}

void Backend::SetEnums(lua_State *L) { skia::SetEnums(L); }

} // namespace luna::backend::skia
