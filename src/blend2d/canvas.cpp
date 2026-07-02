#include "blend2d/canvas.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <blend2d/core/pattern.h>

#include "backend/enums.h"
#include "blend2d/enums.h"

namespace luna::backend::blend2d {

namespace {

constexpr double kMaskPadding = 1.0;
constexpr double kAlignmentEpsilon = 1e-6;

using ResolvedStyle = std::variant<BLRgba32, BLGradient, BLPattern>;

BLContextCreateInfo MakeContextCreateInfo(uint32_t thread_count) {
  BLContextCreateInfo info{};
  info.flags = thread_count > 0 ? BL_CONTEXT_CREATE_FLAG_FALLBACK_TO_SYNC : 0u;
  info.thread_count = thread_count;
  return info;
}

bool BeginContext(BLContext *ctx, BLImage &image, uint32_t thread_count) {
  if (thread_count == 0) {
    return ctx->begin(image) == BL_SUCCESS;
  }
  const BLContextCreateInfo info = MakeContextCreateInfo(thread_count);
  return ctx->begin(image, info) == BL_SUCCESS;
}

bool IsEmptyBox(const BLBoxI &box) {
  return box.x0 >= box.x1 || box.y0 >= box.y1;
}

BLBoxI MakeFullImageBox(const BLImage &image) {
  return BLBoxI(0, 0, image.width(), image.height());
}

BLBoxI IntersectBoxes(const BLBoxI &a, const BLBoxI &b) {
  return BLBoxI(std::max(a.x0, b.x0), std::max(a.y0, b.y0),
      std::min(a.x1, b.x1), std::min(a.y1, b.y1));
}

BLRectI BoxToRect(const BLBoxI &box) {
  return BLRectI(box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
}

BLRectI FullImageRect(const BLImage &image) {
  return BLRectI(0, 0, image.width(), image.height());
}

BLBoxI LocalClipBox(const StackEntry &entry) {
  const BLBoxI local(entry.clip_box.x0 - entry.layer_origin.x,
      entry.clip_box.y0 - entry.layer_origin.y,
      entry.clip_box.x1 - entry.layer_origin.x,
      entry.clip_box.y1 - entry.layer_origin.y);
  return IntersectBoxes(local, MakeFullImageBox(entry.image));
}

bool NearlyEqual(double a, double b) {
  return std::abs(a - b) <= kAlignmentEpsilon;
}

bool NearlyInteger(double value) {
  return NearlyEqual(value, std::round(value));
}

BLPoint MapWindowPointToSurface(
    const BLMatrix2D &window_to_surface_transform, double x, double y) {
  return window_to_surface_transform.map_point(x, y);
}

BLBoxI ExpandBoundingBox(const BLBox &box) {
  return BLBoxI(static_cast<int>(std::floor(box.x0 - kMaskPadding)),
      static_cast<int>(std::floor(box.y0 - kMaskPadding)),
      static_cast<int>(std::ceil(box.x1 + kMaskPadding)),
      static_cast<int>(std::ceil(box.y1 + kMaskPadding)));
}

ResolvedStyle ResolveStyle(const Shader &shader, const BLMatrix2D &transform) {
  return std::visit(
      [&](auto const &value) -> ResolvedStyle {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, BLRgba32>) {
          return value;
        } else if constexpr (std::is_same_v<T, BLGradient>) {
          BLGradient gradient(value);
          gradient.apply_transform(transform);
          return gradient;
        } else {
          BLPattern pattern(value);
          pattern.apply_transform(transform);
          return pattern;
        }
      },
      static_cast<Shader::variant const &>(shader));
}

ResolvedStyle ResolveLocalStyle(
    const Shader &shader, const BLMatrix2D &transform, BLPointI layer_origin) {
  BLMatrix2D local_transform(transform);
  local_transform.translate(-layer_origin.x, -layer_origin.y);
  return ResolveStyle(shader, local_transform);
}

void ConfigureContextForPaint(
    BLContext *ctx, const Paint &paint, double alpha_multiplier = 1.0) {
  ctx->set_comp_op(paint.comp_op);
  ctx->set_global_alpha(paint.alpha * alpha_multiplier);
  ctx->set_gradient_quality(BL_GRADIENT_QUALITY_SMOOTH);
  ctx->set_pattern_quality(
      paint.shader.image_sampling == ImageSamplingMode::kNearest
          ? BL_PATTERN_QUALITY_NEAREST
          : BL_PATTERN_QUALITY_BILINEAR);
}

bool CompOpIsIdentityOnEmpty(BLCompOp comp_op) {
  switch (comp_op) {
  case BL_COMP_OP_SRC_COPY:
  case BL_COMP_OP_SRC_OVER:
  case BL_COMP_OP_PLUS:
    return true;
  default:
    return false;
  }
}

template <typename Fn>
void VisitResolvedStyle(const ResolvedStyle &style, Fn &&fn) {
  std::visit([&](auto const &value) { fn(value); }, style);
}

Paint ResolvePaint(const Paint *paint) {
  Paint resolved;
  resolved.comp_op = BL_COMP_OP_SRC_OVER;
  resolved.shader.MakeSolidColor(0xFF000000u);
  if (paint != nullptr) {
    resolved = *paint;
  }
  return resolved;
}

bool PaintCanReplaceEmptyLayer(const Paint &paint) {
  return CompOpIsIdentityOnEmpty(paint.comp_op) && NearlyEqual(paint.alpha, 1.0);
}

BLPath MakeRectPath(double x, double y, double w, double h) {
  BLPath path;
  path.add_rect(x, y, w, h);
  return path;
}

BLPath MakeRoundRectPath(
    double x, double y, double w, double h, double rx, double ry) {
  BLPath path;
  path.add_round_rect(BLRoundRect(x, y, w, h, rx, ry));
  return path;
}

BLPath BuildDevicePath(const StackEntry &entry, const BLPath &user_path) {
  BLPath path(user_path);
  path.transform(entry.transform);
  return path;
}

BLPath BuildRenderablePath(const StackEntry &entry, const BLPath &user_path,
    const Paint &paint) {
  BLPath path_to_draw(user_path);
  if (std::holds_alternative<Paint::StrokeStyle>(paint.style)) {
    BLPath stroked;
    const auto &stroke = std::get<Paint::StrokeStyle>(paint.style);
    BLApproximationOptions approx = bl_default_approximation_options;
    stroked.add_stroked_path(path_to_draw, stroke.options, approx);
    path_to_draw = std::move(stroked);
  }
  return BuildDevicePath(entry, path_to_draw);
}

BLPath MakeLocalPath(const StackEntry &entry, const BLPath &device_path) {
  BLPath local(device_path);
  local.translate(BLPoint(-entry.layer_origin.x, -entry.layer_origin.y));
  return local;
}

std::optional<BLBoxI> PixelAlignedDeviceRect(
    const StackEntry &entry, double x, double y, double w, double h) {
  const BLMatrix2D &m = entry.transform;
  if (!NearlyEqual(m.m01, 0.0) || !NearlyEqual(m.m10, 0.0)) {
    return std::nullopt;
  }

  const double x0 = m.m00 * x + m.m20;
  const double y0 = m.m11 * y + m.m21;
  const double x1 = m.m00 * (x + w) + m.m20;
  const double y1 = m.m11 * (y + h) + m.m21;
  if (!NearlyInteger(x0) || !NearlyInteger(y0) || !NearlyInteger(x1) ||
      !NearlyInteger(y1)) {
    return std::nullopt;
  }

  return BLBoxI(static_cast<int>(std::lround(std::min(x0, x1))),
      static_cast<int>(std::lround(std::min(y0, y1))),
      static_cast<int>(std::lround(std::max(x0, x1))),
      static_cast<int>(std::lround(std::max(y0, y1))));
}

struct RasterMask {
  BLImage image;
  BLPointI origin;
  BLBoxI bounds;
};

BLBoxI ClipMaskBox(const StackEntry &entry) {
  return BLBoxI(entry.clip_mask_origin.x, entry.clip_mask_origin.y,
      entry.clip_mask_origin.x + entry.clip_mask.width(),
      entry.clip_mask_origin.y + entry.clip_mask.height());
}

BLBoxI DirtyBoundsForPath(const StackEntry &entry, const BLPath &device_path) {
  BLBox bounds_d{};
  if (device_path.get_bounding_box(&bounds_d) != BL_SUCCESS) {
    return entry.clip_box;
  }
  return IntersectBoxes(ExpandBoundingBox(bounds_d), entry.clip_box);
}

std::optional<RasterMask> RasterizePathToMask(
    const StackEntry &entry, const BLPath &device_path, BLFillRule fill_rule,
    uint32_t thread_count) {
  BLBoxI bounds = DirtyBoundsForPath(entry, device_path);
  if (IsEmptyBox(bounds)) {
    return std::nullopt;
  }

  if (!entry.clip_mask.is_empty()) {
    bounds = IntersectBoxes(bounds, ClipMaskBox(entry));
    if (IsEmptyBox(bounds)) {
      return std::nullopt;
    }
  }

  BLImage mask;
  if (mask.create(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0, BL_FORMAT_A8) !=
      BL_SUCCESS) {
    return std::nullopt;
  }

  BLContext mask_ctx;
  if (!BeginContext(&mask_ctx, mask, thread_count)) {
    return std::nullopt;
  }
  mask_ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  mask_ctx.clear_all();
  mask_ctx.set_fill_rule(fill_rule);
  mask_ctx.set_pattern_quality(BL_PATTERN_QUALITY_NEAREST);

  BLPath local(device_path);
  local.translate(BLPoint(-bounds.x0, -bounds.y0));
  if (entry.clip_mask.is_empty()) {
    mask_ctx.fill_path(local, BLRgba32(0xFFFFFFFFu));
  } else {
    const BLRectI src_area(bounds.x0 - entry.clip_mask_origin.x,
        bounds.y0 - entry.clip_mask_origin.y, bounds.x1 - bounds.x0,
        bounds.y1 - bounds.y0);
    BLPattern clip_pattern(entry.clip_mask, src_area, BL_EXTEND_MODE_PAD);
    mask_ctx.fill_path(local, clip_pattern);
  }
  mask_ctx.end();

  return RasterMask{std::move(mask), BLPointI(bounds.x0, bounds.y0), bounds};
}

void DetachContext(Canvas *canvas) {
  if (canvas->ctx.is_valid()) {
    canvas->ctx.end();
  }
}

StackEntry &Current(Canvas *canvas) { return canvas->stack.back(); }

StackEntry const &Current(Canvas const *canvas) { return canvas->stack.back(); }

void ResetClipMask(StackEntry *entry) {
  entry->clip_mask.reset();
  entry->clip_mask_origin = BLPointI(0, 0);
}

void SetEmptyClip(StackEntry *entry) {
  entry->clip_box = BLBoxI(0, 0, 0, 0);
  entry->clip_origin = BLPointI(0, 0);
  ResetClipMask(entry);
}

void SetRectClip(StackEntry *entry, BLBoxI clip_box) {
  entry->clip_box = clip_box;
  entry->clip_origin =
      IsEmptyBox(clip_box) ? BLPointI(0, 0) : BLPointI(clip_box.x0, clip_box.y0);
  ResetClipMask(entry);
}

StackEntry MakeRootEntry(BLImage image, BLMatrix2D transform) {
  StackEntry root;
  root.image = std::move(image);
  root.transform = transform;
  root.layer_origin = BLPointI(0, 0);
  root.dirty_region.Clear();
  SetRectClip(&root, MakeFullImageBox(root.image));
  return root;
}

BLImage CreateClearedImage(int width, int height, uint32_t thread_count) {
  BLImage image(width, height, BL_FORMAT_PRGB32);
  BLContext ctx;
  if (!BeginContext(&ctx, image, thread_count)) {
    image.reset();
    return image;
  }

  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.clear_all();
  ctx.end();
  return image;
}

bool CanAdoptLayerIntoParent(const StackEntry &parent, const StackEntry &layer) {
  if (parent.kind != StackEntry::Kind::kLayer ||
      !parent.dirty_region.IsEmpty()) {
    return false;
  }
  const Paint paint =
      ResolvePaint(layer.layer_paint ? &*layer.layer_paint : nullptr);
  return PaintCanReplaceEmptyLayer(paint);
}

void MarkCurrentDirty(Canvas *canvas, const BLBoxI &dirty_box) {
  const BLBoxI clipped = IntersectBoxes(dirty_box, Current(canvas).clip_box);
  if (IsEmptyBox(clipped)) {
    return;
  }
  Current(canvas).dirty_region.UnionRect(clipped);
}

void MarkCurrentDirty(Canvas *canvas, const Region &region) {
  Region clipped = region;
  clipped.Intersect(Current(canvas).clip_box);
  Current(canvas).dirty_region.Union(clipped);
}

void ReattachContext(Canvas *canvas) {
  DetachContext(canvas);
  if (!canvas->stack.empty() && !Current(canvas).image.is_empty()) {
    if (!BeginContext(
            &canvas->ctx, Current(canvas).image, canvas->thread_count_)) {
      return;
    }
    canvas->ctx.restore_clipping();
    const BLBoxI clip_box = LocalClipBox(Current(canvas));
    if (clip_box != MakeFullImageBox(Current(canvas).image)) {
      canvas->ctx.clip_to_rect(BoxToRect(clip_box));
    }
  }
}

void ApplyClipMaskToState(
    StackEntry *entry, BLImage mask, BLPointI origin, BLBoxI clip_box) {
  if (mask.is_empty() || IsEmptyBox(clip_box)) {
    SetEmptyClip(entry);
    return;
  }

  entry->clip_box = clip_box;
  entry->clip_origin = BLPointI(clip_box.x0, clip_box.y0);
  entry->clip_mask = std::move(mask);
  entry->clip_mask_origin = origin;
}

void SyncContextClip(Canvas *canvas) {
  if (!canvas->ctx.is_valid() || canvas->stack.empty()) {
    return;
  }

  canvas->ctx.restore_clipping();
  const BLBoxI clip_box = LocalClipBox(Current(canvas));
  if (clip_box != MakeFullImageBox(Current(canvas).image)) {
    canvas->ctx.clip_to_rect(BoxToRect(clip_box));
  }
}

void DrawMasked(Canvas *canvas, const Paint &paint, const ResolvedStyle &style,
    RasterMask const &mask) {
  ConfigureContextForPaint(&canvas->ctx, paint);
  const BLRectI area = FullImageRect(mask.image);
  const BLPointI local_origin(mask.origin.x - Current(canvas).layer_origin.x,
      mask.origin.y - Current(canvas).layer_origin.y);
  VisitResolvedStyle(style, [&](auto const &resolved) {
    canvas->ctx.fill_mask(local_origin, mask.image, area, resolved);
  });
  MarkCurrentDirty(canvas, mask.bounds);
}

void DrawDirect(Canvas *canvas, const Paint &paint, const ResolvedStyle &style,
    const BLPath &device_path, BLFillRule fill_rule) {
  const BLBoxI dirty_box = DirtyBoundsForPath(Current(canvas), device_path);
  ConfigureContextForPaint(&canvas->ctx, paint);
  canvas->ctx.set_fill_rule(fill_rule);
  const BLPath local_path = MakeLocalPath(Current(canvas), device_path);
  VisitResolvedStyle(style, [&](auto const &resolved) {
    canvas->ctx.fill_path(local_path, resolved);
  });
  MarkCurrentDirty(canvas, dirty_box);
}

void DrawRenderablePath(Canvas *canvas, const BLPath &user_path,
    BLFillRule fill_rule, Paint const *paint_) {
  const Paint paint = ResolvePaint(paint_);
  BLPath device_path = BuildRenderablePath(Current(canvas), user_path, paint);
  if (Current(canvas).clip_mask.is_empty()) {
    const ResolvedStyle style = ResolveLocalStyle(
        paint.shader, Current(canvas).transform, Current(canvas).layer_origin);
    DrawDirect(canvas, paint, style, device_path, fill_rule);
    return;
  }

  const ResolvedStyle style =
      ResolveStyle(paint.shader, Current(canvas).transform);
  auto mask = RasterizePathToMask(
      Current(canvas), device_path, fill_rule, canvas->thread_count_);
  if (mask.has_value()) {
    DrawMasked(canvas, paint, style, *mask);
  }
}

bool ClipWithPath(
    Canvas *canvas, const BLPath &device_path, BLFillRule fill_rule) {
  auto mask = RasterizePathToMask(
      Current(canvas), device_path, fill_rule, canvas->thread_count_);
  if (!mask.has_value()) {
    SetEmptyClip(&Current(canvas));
    SyncContextClip(canvas);
    return false;
  }
  ApplyClipMaskToState(
      &Current(canvas), std::move(mask->image), mask->origin, mask->bounds);
  SyncContextClip(canvas);
  return !IsEmptyBox(Current(canvas).clip_box);
}

} // namespace

void Paint::SetShader(Shader const &shader_) { shader = shader_; }

void Paint::SetAlpha(double alpha_) { alpha = alpha_; }

void Paint::SetBlendMode(BLCompOp blend_mode) { comp_op = blend_mode; }

void Paint::SetFillStyle() { style = FillStyle{}; }

void Paint::SetStrokeStyle(double stroke_width) {
  StrokeStyle stroke;
  stroke.options.width = stroke_width;
  style = std::move(stroke);
}

void Font::SetSize(double size_) { size = size_; }

void Font::SetFamily(const char *family_) { family = family_; }

void Font::SetStyle(int weight_, int width_, int slant_) {
  weight = static_cast<uint32_t>(weight_);
  width = static_cast<uint32_t>(width_);
  slant = static_cast<uint32_t>(slant_);
}

void Canvas::Init(
    BLImage image, BLFontManager font_manager, BLMatrix2D initial_transform,
    uint32_t thread_count) {
  DetachContext(this);
  font_mgr = std::move(font_manager);
  thread_count_ = thread_count;
  window_to_surface_transform_ = initial_transform;
  stack.clear();
  stack.push_back(MakeRootEntry(std::move(image), initial_transform));
  ReattachContext(this);
}

void Canvas::ResetTopImage(BLImage image, BLMatrix2D initial_transform) {
  DetachContext(this);
  if (stack.empty()) {
    Init(std::move(image), font_mgr, initial_transform, thread_count_);
    return;
  }
  window_to_surface_transform_ = initial_transform;
  stack.clear();
  stack.push_back(MakeRootEntry(std::move(image), initial_transform));
  ReattachContext(this);
}

BLImage Canvas::TakeTopImage() {
  DetachContext(this);
  if (stack.empty()) {
    return BLImage();
  }
  BLImage image = std::move(Current(this).image);
  stack.clear();
  return image;
}

void Canvas::Flush() {
  if (ctx.is_valid()) {
    ctx.flush(BL_CONTEXT_FLUSH_SYNC);
  }
}

void Canvas::Clear(uint32_t color) {
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.set_global_alpha(1.0);
  if (Current(this).clip_mask.is_empty()) {
    const BLBoxI local_clip = LocalClipBox(Current(this));
    if (IsEmptyBox(local_clip)) {
      return;
    }
    if (local_clip == MakeFullImageBox(Current(this).image)) {
      ctx.fill_all(BLRgba32(color));
    } else {
      ctx.fill_rect(BoxToRect(local_clip), BLRgba32(color));
    }
    MarkCurrentDirty(this, Current(this).clip_box);
    return;
  }
  const BLPointI local_origin(
      Current(this).clip_mask_origin.x - Current(this).layer_origin.x,
      Current(this).clip_mask_origin.y - Current(this).layer_origin.y);
  ctx.fill_mask(local_origin, Current(this).clip_mask,
      FullImageRect(Current(this).clip_mask), BLRgba32(color));
  MarkCurrentDirty(this, Current(this).clip_box);
}

void Canvas::Save() {
  DetachContext(this);
  StackEntry entry = Current(this);
  entry.kind = StackEntry::Kind::kState;
  entry.layer_paint.reset();
  entry.image = std::move(Current(this).image);
  stack.push_back(std::move(entry));
  ReattachContext(this);
}

void Canvas::SaveLayer(Paint const *paint) {
  DetachContext(this);
  const StackEntry &parent = Current(this);
  const Paint layer_paint = ResolvePaint(paint);
  StackEntry layer;
  layer.kind = StackEntry::Kind::kLayer;
  layer.transform = parent.transform;
  layer.clip_origin = parent.clip_origin;
  layer.clip_box = parent.clip_box;
  layer.layer_paint = layer_paint;
  layer.dirty_region.Clear();
  const BLBoxI local_clip = parent.clip_box;
  layer.layer_origin = BLPointI(local_clip.x0, local_clip.y0);
  layer.image = CreateClearedImage(std::max(1, local_clip.x1 - local_clip.x0),
      std::max(1, local_clip.y1 - local_clip.y0), thread_count_);
  if (!parent.clip_mask.is_empty()) {
    layer.clip_mask = parent.clip_mask;
    layer.clip_mask_origin = parent.clip_mask_origin;
  }

  stack.push_back(std::move(layer));
  ReattachContext(this);
}

void Canvas::Restore() {
  if (stack.size() <= 1) {
    return;
  }

  DetachContext(this);
  StackEntry top = std::move(stack.back());
  stack.pop_back();
  if (top.kind == StackEntry::Kind::kState) {
    Current(this).image = std::move(top.image);
    Current(this).dirty_region = std::move(top.dirty_region);
    ReattachContext(this);
    return;
  }

  if (top.image.is_empty() || top.dirty_region.IsEmpty()) {
    ReattachContext(this);
    return;
  }

  if (CanAdoptLayerIntoParent(Current(this), top)) {
    Current(this).image = std::move(top.image);
    Current(this).layer_origin = top.layer_origin;
    Current(this).dirty_region.Union(top.dirty_region);
    ReattachContext(this);
    return;
  }

  ReattachContext(this);

  Paint paint = ResolvePaint(top.layer_paint ? &*top.layer_paint : nullptr);
  ConfigureContextForPaint(&ctx, paint);
  for (const BLBoxI &dirty_rect : top.dirty_region.Rects()) {
    const BLPointI dst_origin(dirty_rect.x0 - Current(this).layer_origin.x,
        dirty_rect.y0 - Current(this).layer_origin.y);
    const BLRectI src_area(dirty_rect.x0 - top.layer_origin.x,
        dirty_rect.y0 - top.layer_origin.y, dirty_rect.x1 - dirty_rect.x0,
        dirty_rect.y1 - dirty_rect.y0);
    ctx.blit_image(dst_origin, top.image, src_area);
  }
  MarkCurrentDirty(this, top.dirty_region);
}

void Canvas::Translate(double dx, double dy) {
  Current(this).transform.translate(dx, dy);
}

void Canvas::Scale(double sx, double sy) {
  Current(this).transform.scale(sx, sy);
}

void Canvas::Skew(double sx, double sy) {
  Current(this).transform.skew(sx, sy);
}

void Canvas::Rotate(double radians) { Current(this).transform.rotate(radians); }

bool Canvas::Snapshot(Image *image, std::string *error) {
  Flush();
  if (stack.empty()) {
    if (error != nullptr) {
      *error = "snapshot: canvas does not support snapshots";
    }
    return false;
  }

  if (image->image.assign_deep(Current(this).image) != BL_SUCCESS) {
    if (error != nullptr) {
      *error = "snapshot: failed to copy image";
    }
    return false;
  }
  return true;
}

bool Canvas::HitTestRect(
    double x, double y, double w, double h, double hit_x, double hit_y) {
  BLPath path = MakeRectPath(x, y, w, h);
  path.transform(Current(this).transform);
  const BLPoint p =
      MapWindowPointToSurface(window_to_surface_transform_, hit_x, hit_y);
  return path.hit_test(p, BL_FILL_RULE_NON_ZERO) != BL_HIT_TEST_OUT;
}

bool Canvas::HitTestPath(Path const &path, double hit_x, double hit_y) {
  BLPath test = path.bl;
  test.transform(Current(this).transform);
  const BLPoint p =
      MapWindowPointToSurface(window_to_surface_transform_, hit_x, hit_y);
  return test.hit_test(p, path.fill_rule) != BL_HIT_TEST_OUT;
}

void Canvas::DrawRect(
    double x, double y, double w, double h, Paint const *paint_) {
  DrawRenderablePath(
      this, MakeRectPath(x, y, w, h), BL_FILL_RULE_NON_ZERO, paint_);
}

void Canvas::DrawRoundRect(double x, double y, double w, double h, double rx,
    double ry, Paint const *paint_) {
  DrawRenderablePath(this, MakeRoundRectPath(x, y, w, h, rx, ry),
      BL_FILL_RULE_NON_ZERO, paint_);
}

void Canvas::DrawPath(const Path &path, Paint *const paint_) {
  DrawRenderablePath(this, path.bl, path.fill_rule, paint_);
}

void Canvas::ClipRect(double x, double y, double w, double h) {
  if (Current(this).clip_mask.is_empty()) {
    const std::optional<BLBoxI> aligned =
        PixelAlignedDeviceRect(Current(this), x, y, w, h);
    if (aligned.has_value()) {
      SetRectClip(
          &Current(this), IntersectBoxes(Current(this).clip_box, *aligned));
      SyncContextClip(this);
      return;
    }
  }

  BLPath path = MakeRectPath(x, y, w, h);
  BLPath device_path = BuildDevicePath(Current(this), path);
  ClipWithPath(this, device_path, BL_FILL_RULE_NON_ZERO);
}

void Canvas::ClipRoundRect(
    double x, double y, double w, double h, double rx, double ry) {
  BLPath path = MakeRoundRectPath(x, y, w, h, rx, ry);
  BLPath device_path = BuildDevicePath(Current(this), path);
  ClipWithPath(this, device_path, BL_FILL_RULE_NON_ZERO);
}

void Canvas::ClipPath(const Path &path) {
  BLPath device_path = BuildDevicePath(Current(this), path.bl);
  ClipWithPath(this, device_path, path.fill_rule);
}

Backend::FillType Backend::DefaultFillType() { return BL_FILL_RULE_NON_ZERO; }

Backend::ImageSamplingMode Backend::DefaultImageSamplingMode() {
  return ImageSamplingMode::kLinear;
}

Backend::TileMode Backend::DefaultTileMode() { return BL_EXTEND_MODE_PAD; }

Backend::TextAlign Backend::DefaultTextAlign() { return TextAlign::kLeft; }

int Backend::DefaultFontWeight() {
  return static_cast<int>(BL_FONT_WEIGHT_NORMAL);
}

int Backend::DefaultFontWidth() {
  return static_cast<int>(BL_FONT_STRETCH_NORMAL);
}

int Backend::DefaultFontSlant() {
  return static_cast<int>(BL_FONT_STYLE_NORMAL);
}

void Backend::SetEnums(lua_State *L) { blend2d::SetEnums(L); }

} // namespace luna::backend::blend2d
