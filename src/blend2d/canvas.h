#ifndef LUNA_BLEND2D_CANVAS_H
#define LUNA_BLEND2D_CANVAS_H

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <blend2d/blend2d.h>

#include "backend/canvas.h"
#include "blend2d/region.h"

namespace luna::backend::blend2d {

struct Canvas;

struct Image {
  static constexpr const char *MT = "luna.Image";
  BLImage image;
};

struct Path {
  static constexpr const char *MT = "luna.Path";
  BLPath bl;
  BLFillRule fill_rule = BL_FILL_RULE_NON_ZERO;

  Path() = default;
  explicit Path(BLFillRule fill_type) : fill_rule(fill_type) {}

  void SetFillType(BLFillRule fill_type);

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
};

struct Shader : public std::variant<BLRgba32, BLGradient, BLPattern> {
  static constexpr const char *MT = "luna.Shader";
  using variant = std::variant<BLRgba32, BLGradient, BLPattern>;
  using variant::operator=;

  ImageSamplingMode image_sampling = ImageSamplingMode::kLinear;

  void MakeSolidColor(uint32_t color);
  bool MakeLinearGradient(double x0, double y0, double x1, double y1,
      std::span<const uint32_t> colors, std::span<const float> positions,
      BLExtendMode tile_mode, std::string *error);
  bool MakeRadialGradient(double cx, double cy, double radius,
      std::span<const uint32_t> colors, std::span<const float> positions,
      BLExtendMode tile_mode, std::string *error);
  void MakeImage(Image *image, ImageSamplingMode sampling,
      BLExtendMode tile_mode, std::string *error);
};

// Roughly equivalent to a fragment shader.
struct Paint {
  static constexpr const char *MT = "luna.Paint";

  struct FillStyle {};
  struct StrokeStyle {
    BLStrokeOptions options;
  };
  using Style = std::variant<FillStyle, StrokeStyle>;
  // The source from which to sample fragments.
  // constant, gradient, or image pattern.
  Shader shader;
  double alpha = 1.0;
  // TODO: implement filters
  // Composition op when the fragments land on the image.
  BLCompOp comp_op = BL_COMP_OP_SRC_OVER;
  Style style = FillStyle{};

  void SetShader(Shader const &shader);
  void SetAlpha(double alpha);
  void SetBlendMode(BLCompOp blend_mode);
  void SetFillStyle();
  void SetStrokeStyle(double stroke_width);
};

struct Font {
  static constexpr const char *MT = "luna.Font";
  double size = 0.0;
  std::string family;
  uint32_t weight = BL_FONT_WEIGHT_NORMAL;
  uint32_t width = BL_FONT_STRETCH_NORMAL;
  uint32_t slant = BL_FONT_STYLE_NORMAL;

  void SetSize(double size);
  void SetFamily(const char *family);
  void SetStyle(int weight, int width, int slant);
};

struct StackEntry {
  enum class Kind {
    kState,
    kLayer,
  };

  Kind kind = Kind::kState;
  BLImage image;
  BLMatrix2D transform;
  BLBoxI clip_box;
  BLPointI layer_origin;
  BLPointI clip_origin;
  BLPointI clip_mask_origin;
  BLImage clip_mask;
  std::optional<Paint> layer_paint;
  Region dirty_region;
};

enum class TextAlign : uint32_t {
  kLeft = 0,
  kCenter = 1,
  kRight = 2,
};

struct Paragraph {
  static constexpr const char *MT = "luna.Paragraph";

  struct Style {
    Font font;
    uint32_t color = 0xFFFFFFFFu;
  };

  struct Segment {
    Style style;
    std::string text;
  };

  struct Run {
    BLPath path;
    Font font;
    std::string text;
    uint32_t color = 0xFFFFFFFFu;
    bool whitespace = false;
    bool synthetic = false;
    size_t start_index = 0;
    size_t end_index = 0;
    double x = 0.0;
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    double leading = 0.0;
  };

  struct Line {
    std::vector<Run> runs;
    size_t start_index = 0;
    size_t end_index = 0;
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    double leading = 0.0;
    double left = 0.0;
    double baseline = 0.0;
  };

  bool valid = false;
  BLFontManager font_mgr;
  Style base_style;
  TextAlign align = TextAlign::kLeft;
  std::optional<size_t> max_lines;
  std::string ellipsis;
  std::string last_error;
  double layout_width = 0.0;
  ParagraphMetrics metrics;
  std::vector<Style> style_stack;
  std::vector<Segment> segments;
  std::vector<Line> lines;

  bool IsValid() const;
  std::string const &Error() const;
  ParagraphMetrics Measure() const;
  bool Init(Canvas const &canvas, Font const &font, uint32_t color,
      TextAlign align, std::optional<size_t> max_lines, const char *ellipsis,
      std::string *error);
  void PushStyle(Font const &font, uint32_t color);
  void PopStyle();
  void AddText(std::string_view text);
  void Layout(double width);
};

struct Canvas {
  static constexpr const char *MT = "luna.Canvas";

  BLContext ctx;
  BLFontManager font_mgr;
  std::vector<StackEntry> stack;
  uint32_t thread_count_ = 0;
  BLMatrix2D window_to_surface_transform_ = BLMatrix2D::make_identity();

  void Init(BLImage image, BLFontManager font_manager,
      BLMatrix2D initial_transform = BLMatrix2D::make_identity(),
      uint32_t thread_count = 0);
  void ResetTopImage(BLImage image, BLMatrix2D initial_transform);
  BLImage TakeTopImage();
  void Flush();

  void Clear(uint32_t color);
  void Save();
  void SaveLayer(Paint const *paint);
  void Restore();
  void Translate(double dx, double dy);
  void Scale(double sx, double sy);
  void Skew(double sx, double sy);
  void Rotate(double radians);

  bool Snapshot(Image *image, std::string *error);

  bool HitTestRect(
      double x, double y, double w, double h, double hit_x, double hit_y);
  bool HitTestPath(Path const &path, double hit_x, double hit_y);

  void DrawRect(double x, double y, double w, double h, Paint const *paint);
  void DrawRoundRect(double x, double y, double w, double h, double rx,
      double ry, Paint const *paint);
  void DrawPath(Path const &path, Paint *const paint);

  void ClipRect(double x, double y, double w, double h);
  void ClipRoundRect(
      double x, double y, double w, double h, double rx, double ry);
  void ClipPath(Path const &path);

  bool MeasureText(std::string_view text, Font const &font, Paint const *paint,
      TextMetrics *metrics, std::string *error);
  bool DrawText(std::string_view text, double x, double y, Font const &font,
      Paint const *paint, std::string *error);
  void DrawParagraph(Paragraph const &paragraph, double x, double y);
};

struct Backend {
  using Canvas = blend2d::Canvas;
  using Image = blend2d::Image;
  using Path = blend2d::Path;
  using Paint = blend2d::Paint;
  using Font = blend2d::Font;
  using Shader = blend2d::Shader;
  using Paragraph = blend2d::Paragraph;
  using FillType = BLFillRule;
  using BlendMode = BLCompOp;
  using ImageSamplingMode = ImageSamplingMode;
  using TileMode = BLExtendMode;
  using TextAlign = blend2d::TextAlign;

  static FillType DefaultFillType();
  static ImageSamplingMode DefaultImageSamplingMode();
  static TileMode DefaultTileMode();
  static TextAlign DefaultTextAlign();
  static int DefaultFontWeight();
  static int DefaultFontWidth();
  static int DefaultFontSlant();
  static constexpr bool SupportsSvgPathParsing() { return false; }
  static constexpr bool SupportsSvgPathSerialization() { return false; }
  static void SetEnums(lua_State *L);
};

} // namespace luna::backend::blend2d

#endif
