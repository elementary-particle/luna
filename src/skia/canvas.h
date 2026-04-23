#ifndef LUNA_SKIA_CANVAS_H
#define LUNA_SKIA_CANVAS_H

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <skia/core/SkBlendMode.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkClipOp.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkMatrix.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkPath.h>
#include <skia/core/SkPathBuilder.h>
#include <skia/core/SkPathTypes.h>
#include <skia/core/SkShader.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkTileMode.h>
#include <skia/core/SkTypeface.h>
#include <skia/modules/skparagraph/include/FontCollection.h>
#include <skia/modules/skparagraph/include/Paragraph.h>
#include <skia/modules/skparagraph/include/ParagraphBuilder.h>

#include "backend/canvas.h"

struct lua_State;

namespace luna::backend::skia {

using ::skia::textlayout::FontCollection;
using SkParagraph = ::skia::textlayout::Paragraph;
using ::skia::textlayout::ParagraphBuilder;
using ::skia::textlayout::TextAlign;

struct Image {
  static constexpr const char *MT = "luna.Image";
  sk_sp<SkImage> sk;

  Image() = default;
  explicit Image(sk_sp<SkImage> image) : sk(std::move(image)) {}
};

struct Path {
  static constexpr const char *MT = "luna.Path";
  SkPathBuilder sk;

  Path() = default;
  explicit Path(SkPathFillType fill_type) : sk(fill_type) {}
  explicit Path(const SkPath &path) : sk(path) {}
  Path(const Path &) = default;
  Path(Path &&) = default;
  Path &operator=(const Path &) = default;
  Path &operator=(Path &&) = default;

  void SetFillType(SkPathFillType fill_type);

  void MoveTo(double x, double y);
  void LineTo(double x, double y);
  void QuadTo(double x1, double y1, double x2, double y2);
  void SmoothQuadTo(double x2, double y2);
  void CubicTo(
      double x1, double y1, double x2, double y2, double x3, double y3);
  void SmoothCubicTo(double x2, double y2, double x3, double y3);
  void ArcTo(double rx, double ry, double x_axis_rotation, bool large_arc_flag,
      bool sweep_flag, double x1, double y1);
  void Close();
  void Reset();

  static std::optional<Path> FromSvgString(const char *svg);
  std::string ToSvgString(bool relative) const;

  SkPath snapshot() const { return sk.snapshot(); }
};

struct Shader {
  static constexpr const char *MT = "luna.Shader";
  sk_sp<SkShader> sk;

  Shader() = default;
  explicit Shader(sk_sp<SkShader> shader) : sk(std::move(shader)) {}
  void MakeSolidColor(uint32_t color);
  bool MakeLinearGradient(double x0, double y0, double x1, double y1,
      std::span<const uint32_t> colors, std::span<const float> positions,
      SkTileMode tile_mode, std::string *error);
  bool MakeRadialGradient(double cx, double cy, double radius,
      std::span<const uint32_t> colors, std::span<const float> positions,
      SkTileMode tile_mode, std::string *error);
  void MakeImage(Image *image, ImageSamplingMode sampling, SkTileMode tile_mode,
      std::string *error);
};

struct Paragraph;

class Canvas {
public:
  struct Paint {
    static constexpr const char *MT = "luna.Paint";
    SkPaint sk;

    Paint() { sk.setAntiAlias(true); }
    void SetShader(Shader const &shader);
    void SetAlpha(double alpha);
    void SetBlendMode(SkBlendMode blend_mode);
    void SetFillStyle();
    void SetStrokeStyle(double stroke_width);
  };

  struct Font {
    static constexpr const char *MT = "luna.Font";
    SkScalar size = 0;
    SkString family_name;
    SkFontStyle style = SkFontStyle();

    void SetSize(double size_);
    void SetFamily(const char *family);
    void SetStyle(int weight, int width, int slant);
  };

private:
  SkCanvas *sk_ = nullptr;
  sk_sp<SkSurface> surface_;
  sk_sp<SkFontMgr> font_mgr_;
  SkMatrix window_to_surface_matrix_ = SkMatrix::I();

public:
  static constexpr const char *MT = "luna.Canvas";

  void set_sk(SkCanvas *sk) {
    surface_.reset();
    sk_ = sk;
    window_to_surface_matrix_ = SkMatrix::I();
  }

  SkCanvas *sk() const { return sk_; }

  void set_font_manager(sk_sp<SkFontMgr> font_mgr) {
    font_mgr_ = std::move(font_mgr);
  }
  void set_window_to_surface_matrix(const SkMatrix &matrix) {
    window_to_surface_matrix_ = matrix;
  }
  sk_sp<SkFontMgr> font_mgr() const { return font_mgr_; }
  sk_sp<SkSurface> surface() const { return surface_; }

  void set_surface(sk_sp<SkSurface> surface) {
    surface_ = std::move(surface);
    sk_ = surface_ ? surface_->getCanvas() : nullptr;
    window_to_surface_matrix_ = SkMatrix::I();
  }

  void Clear(uint32_t color);
  void Save();
  void SaveLayer(Paint const *paint);
  void Restore();
  void Translate(double dx, double dy);
  void Scale(double sx, double sy);
  void Skew(double sx, double sy);
  void Rotate(double radians);

  bool Snapshot(Image *image, std::string *error) const;

  bool HitTestRect(
      double x, double y, double w, double h, double hit_x, double hit_y);
  bool HitTestPath(const Path &path, double hit_x, double hit_y);

  void DrawRect(double x, double y, double w, double h, const Paint *paint);
  void DrawRoundRect(double x, double y, double w, double h, double rx,
      double ry, const Paint *paint);
  void DrawPath(const Path &path, const Paint *paint);

  void ClipRect(double x, double y, double w, double h);
  void ClipRoundRect(
      double x, double y, double w, double h, double rx, double ry);
  void ClipPath(const Path &path);

  bool MeasureText(std::string_view text, const Font &font, const Paint *paint,
      TextMetrics *metrics, std::string *error);
  bool DrawText(std::string_view text, double x, double y, const Font &font,
      const Paint *paint, std::string *error);
  void DrawParagraph(const Paragraph &paragraph, double x, double y);

  Canvas() = default;
};

struct Paragraph {
  static constexpr const char *MT = "luna.Paragraph";
  sk_sp<FontCollection> font_collection;
  std::unique_ptr<ParagraphBuilder> sk_builder;
  std::unique_ptr<SkParagraph> sk;
  double layout_width = 0.0;
  std::string last_error;

  Paragraph() = default;

  bool IsValid() const;
  std::string const &Error() const;
  ParagraphMetrics Measure() const;

  bool Init(Canvas const &canvas, Canvas::Font const &font, uint32_t color,
      TextAlign align, std::optional<size_t> max_lines, const char *ellipsis,
      std::string *error);
  void PushStyle(Canvas::Font const &font, uint32_t color);
  void PopStyle();
  void AddText(std::string_view text);
  void Layout(double width);
};

void SetEnums(lua_State *L);

struct Backend {
  using Canvas = skia::Canvas;
  using Image = skia::Image;
  using Path = skia::Path;
  using Paint = skia::Canvas::Paint;
  using Font = skia::Canvas::Font;
  using Shader = skia::Shader;
  using Paragraph = skia::Paragraph;
  using FillType = SkPathFillType;
  using BlendMode = SkBlendMode;
  using ImageSamplingMode = ImageSamplingMode;
  using TileMode = SkTileMode;
  using TextAlign = skia::TextAlign;

  static FillType DefaultFillType();
  static ImageSamplingMode DefaultImageSamplingMode();
  static TileMode DefaultTileMode();
  static TextAlign DefaultTextAlign();
  static int DefaultFontWeight();
  static int DefaultFontWidth();
  static int DefaultFontSlant();
  static constexpr bool SupportsSvgPathParsing() { return true; }
  static constexpr bool SupportsSvgPathSerialization() { return true; }
  static void SetEnums(lua_State *L);
};

} // namespace luna::backend::skia

#endif
