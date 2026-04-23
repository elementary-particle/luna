#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <skia/core/SkAlphaType.h>
#include <skia/core/SkColorType.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkMatrix.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkStream.h>
#include <skia/core/SkSurface.h>
#include <skia/encode/SkPngEncoder.h>

#include "backend/enums.h"
#include "blend2d/canvas.h"
#include "blend2d/font_manager.h"
#include "skia/canvas.h"
#include "skia/font_manager.h"

namespace {

constexpr int kWidth = 176;
constexpr int kHeight = 128;
constexpr int kPatternSize = 18;
constexpr int kDefaultIterations = 200;
constexpr int kDefaultOpsPerScene = 32;
constexpr int kMaxFailuresToDump = 10;
constexpr uint64_t kDefaultBaseSeed = 0xC0FFEE123456789ULL;
constexpr double kAllowedErrorRatio = 0.01;

struct Random {
  explicit Random(uint64_t seed) : engine(seed) {}

  int Int(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(engine);
  }

  double Real(double lo, double hi) {
    return std::uniform_real_distribution<double>(lo, hi)(engine);
  }

  bool Chance(double probability) {
    return std::bernoulli_distribution(probability)(engine);
  }

  std::mt19937_64 engine;
};

struct PixelBuffer {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::vector<uint8_t> bytes;
};

struct DiffSummary {
  int max_channel_delta = 0;
  int max_x = -1;
  int max_y = -1;
  size_t pixels_over_2 = 0;
  size_t pixels_over_8 = 0;
  int over_8_min_x = 0;
  int over_8_min_y = 0;
  int over_8_max_x = -1;
  int over_8_max_y = -1;
};

struct FuzzConfig {
  int iterations = kDefaultIterations;
  int ops_per_scene = kDefaultOpsPerScene;
  uint64_t base_seed = kDefaultBaseSeed;
};

template <typename Backend> struct ImageSize;

template <> struct ImageSize<luna::backend::skia::Backend> {
  static int Width(const luna::backend::skia::Image &image) {
    return image.sk ? image.sk->width() : 0;
  }
  static int Height(const luna::backend::skia::Image &image) {
    return image.sk ? image.sk->height() : 0;
  }
};

template <> struct ImageSize<luna::backend::blend2d::Backend> {
  static int Width(const luna::backend::blend2d::Image &image) {
    return image.image.width();
  }
  static int Height(const luna::backend::blend2d::Image &image) {
    return image.image.height();
  }
};

template <typename Backend> typename Backend::TileMode RepeatTileMode();

template <>
luna::backend::skia::Backend::TileMode
RepeatTileMode<luna::backend::skia::Backend>() {
  return SkTileMode::kRepeat;
}

template <>
luna::backend::blend2d::Backend::TileMode
RepeatTileMode<luna::backend::blend2d::Backend>() {
  return BL_EXTEND_MODE_REPEAT;
}

template <typename Backend> typename Backend::FillType EvenOddFillType();

template <>
luna::backend::skia::Backend::FillType
EvenOddFillType<luna::backend::skia::Backend>() {
  return SkPathFillType::kEvenOdd;
}

template <>
luna::backend::blend2d::Backend::FillType
EvenOddFillType<luna::backend::blend2d::Backend>() {
  return BL_FILL_RULE_EVEN_ODD;
}

template <typename Backend> struct CanvasFactory;

template <> struct CanvasFactory<luna::backend::skia::Backend> {
  static luna::backend::skia::Canvas MakeCanvas(int width, int height) {
    auto surface = SkSurfaces::Raster(SkImageInfo::Make(
        width, height, kN32_SkColorType, kPremul_SkAlphaType));
    if (!surface) {
      throw std::runtime_error("failed to create Skia raster surface");
    }

    luna::backend::skia::Canvas canvas;
    canvas.set_surface(std::move(surface));
    canvas.set_font_manager(luna::backend::skia::MakeRuntimeFontManager());
    return canvas;
  }
};

template <> struct CanvasFactory<luna::backend::blend2d::Backend> {
  static luna::backend::blend2d::Canvas MakeCanvas(int width, int height) {
    BLImage image(width, height, BL_FORMAT_PRGB32);
    if (image.is_empty()) {
      throw std::runtime_error("failed to create Blend2D image");
    }

    BLContext ctx(image);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0u));
    ctx.end();

    luna::backend::blend2d::Canvas canvas;
    canvas.Init(
        std::move(image), luna::backend::blend2d::MakeRuntimeFontManager());
    return canvas;
  }
};

template <typename Backend> void Require(bool ok, const std::string &message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

double RandomX(Random *rng) { return rng->Real(-20.0, kWidth + 20.0); }
double RandomY(Random *rng) { return rng->Real(-20.0, kHeight + 20.0); }
double RandomW(Random *rng) { return rng->Real(6.0, 64.0); }
double RandomH(Random *rng) { return rng->Real(6.0, 56.0); }

uint32_t RandomColor(
    Random *rng, uint8_t alpha_lo = 255, uint8_t alpha_hi = 255) {
  const uint32_t a = static_cast<uint32_t>(rng->Int(alpha_lo, alpha_hi));
  const uint32_t r = static_cast<uint32_t>(rng->Int(0, 255));
  const uint32_t g = static_cast<uint32_t>(rng->Int(0, 255));
  const uint32_t b = static_cast<uint32_t>(rng->Int(0, 255));
  return (a << 24) | (r << 16) | (g << 8) | b;
}

std::vector<uint32_t> RandomGradientColors(Random *rng) {
  const int count = rng->Int(2, 4);
  std::vector<uint32_t> colors;
  colors.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    colors.push_back(RandomColor(rng, 160, 255));
  }
  return colors;
}

std::vector<float> RandomGradientStops(Random *rng, int count) {
  std::vector<float> positions;
  positions.reserve(static_cast<size_t>(count));
  positions.push_back(0.0f);
  for (int i = 1; i < count - 1; ++i) {
    positions.push_back(static_cast<float>(rng->Real(0.0, 1.0)));
  }
  positions.push_back(1.0f);
  std::sort(positions.begin(), positions.end());
  return positions;
}

template <typename Backend>
typename Backend::Paint MakeSolidPaint(uint32_t color) {
  using Paint = typename Backend::Paint;
  using Shader = typename Backend::Shader;

  Shader shader;
  shader.MakeSolidColor(color);
  Paint paint;
  paint.SetShader(shader);
  return paint;
}

template <typename Backend>
typename Backend::Paint MakeRandomSolidPaint(Random *rng, bool maybe_stroke) {
  typename Backend::Paint paint = MakeSolidPaint<Backend>(RandomColor(rng));
  if (maybe_stroke && rng->Chance(0.35)) {
    paint.SetStrokeStyle(rng->Real(1.0, 4.0));
  }
  return paint;
}

template <typename Backend>
typename Backend::Paint MakeRandomLinearGradientPaint(Random *rng) {
  typename Backend::Paint paint;
  typename Backend::Shader shader;
  std::string error;
  const auto colors = RandomGradientColors(rng);
  const auto positions =
      RandomGradientStops(rng, static_cast<int>(colors.size()));
  const double x0 = rng->Real(-24.0, 24.0);
  const double y0 = rng->Real(-24.0, 24.0);
  const double x1 = x0 + rng->Real(12.0, 64.0);
  const double y1 = y0 + rng->Real(-24.0, 24.0);
  Require<Backend>(shader.MakeLinearGradient(x0, y0, x1, y1, colors, positions,
                       Backend::DefaultTileMode(), &error),
      error.empty() ? "linear gradient failed" : error);
  paint.SetShader(shader);
  return paint;
}

template <typename Backend>
typename Backend::Paint MakeRandomRadialGradientPaint(Random *rng) {
  typename Backend::Paint paint;
  typename Backend::Shader shader;
  std::string error;
  const auto colors = RandomGradientColors(rng);
  const auto positions =
      RandomGradientStops(rng, static_cast<int>(colors.size()));
  const double cx = rng->Real(-12.0, 12.0);
  const double cy = rng->Real(-12.0, 12.0);
  const double radius = rng->Real(8.0, 40.0);
  Require<Backend>(shader.MakeRadialGradient(cx, cy, radius, colors, positions,
                       Backend::DefaultTileMode(), &error),
      error.empty() ? "radial gradient failed" : error);
  paint.SetShader(shader);
  return paint;
}

template <typename Backend> typename Backend::Image MakeSnapshotPattern() {
  using Canvas = typename Backend::Canvas;
  using Image = typename Backend::Image;
  using Paint = typename Backend::Paint;

  Canvas canvas =
      CanvasFactory<Backend>::MakeCanvas(kPatternSize, kPatternSize);
  canvas.Clear(0x00000000u);

  Paint teal = MakeSolidPaint<Backend>(0xFF157A6Eu);
  canvas.DrawRect(0, 0, kPatternSize, kPatternSize, &teal);

  Paint cream = MakeSolidPaint<Backend>(0xFFF9EBC7u);
  canvas.DrawRect(0, 0, kPatternSize / 2, kPatternSize / 2, &cream);
  canvas.DrawRect(kPatternSize / 2, kPatternSize / 2, kPatternSize / 2,
      kPatternSize / 2, &cream);

  Paint accent = MakeSolidPaint<Backend>(0xFFE06D06u);
  canvas.DrawRoundRect(3, 3, 12, 12, 3, 3, &accent);

  Image image;
  std::string error;
  Require<Backend>(canvas.Snapshot(&image, &error),
      error.empty() ? "snapshot pattern failed" : error);
  return image;
}

template <typename Backend>
typename Backend::Paint MakeImagePaint(typename Backend::Image *pattern,
    luna::backend::ImageSamplingMode sampling) {
  typename Backend::Paint paint;
  typename Backend::Shader shader;
  std::string error;
  shader.MakeImage(pattern, sampling, RepeatTileMode<Backend>(), &error);
  Require<Backend>(error.empty(), error);
  paint.SetShader(shader);
  return paint;
}

template <typename Backend> typename Backend::Path MakeRandomPath(Random *rng) {
  using Path = typename Backend::Path;

  Path path;
  double x = RandomX(rng);
  double y = RandomY(rng);
  path.MoveTo(x, y);
  const int segments = rng->Int(2, 5);
  for (int i = 0; i < segments; ++i) {
    const int kind = rng->Int(0, 2);
    const double x1 = RandomX(rng);
    const double y1 = RandomY(rng);
    if (kind == 0) {
      path.LineTo(x1, y1);
    } else if (kind == 1) {
      path.QuadTo(RandomX(rng), RandomY(rng), x1, y1);
    } else {
      path.CubicTo(
          RandomX(rng), RandomY(rng), RandomX(rng), RandomY(rng), x1, y1);
    }
  }
  if (rng->Chance(0.65)) {
    path.Close();
  }
  return path;
}

template <typename Backend> void VerifyHitTesting() {
  using Canvas = typename Backend::Canvas;
  using Path = typename Backend::Path;

  Canvas canvas = CanvasFactory<Backend>::MakeCanvas(kWidth, kHeight);

  Require<Backend>(canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 25.0, 35.0),
      "hit_test_rect should report inside points");
  Require<Backend>(!canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 5.0, 35.0),
      "hit_test_rect should reject outside points");

  canvas.Save();
  canvas.Translate(15.0, -5.0);
  Require<Backend>(canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 40.0, 30.0),
      "hit_test_rect should honor canvas transforms");
  Require<Backend>(!canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 20.0, 30.0),
      "hit_test_rect transformed outside point should miss");
  canvas.Restore();

  Path ring(EvenOddFillType<Backend>());
  ring.MoveTo(20.0, 20.0);
  ring.LineTo(80.0, 20.0);
  ring.LineTo(80.0, 80.0);
  ring.LineTo(20.0, 80.0);
  ring.Close();
  ring.MoveTo(35.0, 35.0);
  ring.LineTo(65.0, 35.0);
  ring.LineTo(65.0, 65.0);
  ring.LineTo(35.0, 65.0);
  ring.Close();

  Require<Backend>(canvas.HitTestPath(ring, 25.0, 25.0),
      "hit_test_path should report points inside the filled region");
  Require<Backend>(!canvas.HitTestPath(ring, 50.0, 50.0),
      "hit_test_path should respect even-odd holes");
  Require<Backend>(!canvas.HitTestPath(ring, 90.0, 90.0),
      "hit_test_path should reject outside points");

  canvas.Save();
  canvas.Translate(10.0, 15.0);
  Require<Backend>(canvas.HitTestPath(ring, 35.0, 40.0),
      "hit_test_path should honor canvas transforms");
  Require<Backend>(!canvas.HitTestPath(ring, 60.0, 65.0),
      "hit_test_path transformed hole point should miss");
  canvas.Restore();
}

void VerifySkiaWindowCoordinateHitTesting() {
  using Backend = luna::backend::skia::Backend;
  using Canvas = typename Backend::Canvas;
  using Path = typename Backend::Path;

  Canvas canvas = CanvasFactory<Backend>::MakeCanvas(kWidth * 2, kHeight * 2);
  canvas.sk()->scale(2.0f, 2.0f);
  canvas.set_window_to_surface_matrix(SkMatrix::Scale(2.0f, 2.0f));

  Require<Backend>(canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 25.0, 35.0),
      "scaled Skia hit_test_rect should accept logical window coordinates");
  Require<Backend>(!canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 5.0, 35.0),
      "scaled Skia hit_test_rect should reject logical window misses");

  canvas.Save();
  canvas.Translate(15.0, -5.0);
  Require<Backend>(canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 40.0, 30.0),
      "scaled Skia hit_test_rect should honor canvas transforms");
  Require<Backend>(!canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 20.0, 30.0),
      "scaled Skia hit_test_rect should miss transformed logical points");
  canvas.Restore();

  Path ring(EvenOddFillType<Backend>());
  ring.MoveTo(20.0, 20.0);
  ring.LineTo(80.0, 20.0);
  ring.LineTo(80.0, 80.0);
  ring.LineTo(20.0, 80.0);
  ring.Close();
  ring.MoveTo(35.0, 35.0);
  ring.LineTo(65.0, 35.0);
  ring.LineTo(65.0, 65.0);
  ring.LineTo(35.0, 65.0);
  ring.Close();

  Require<Backend>(canvas.HitTestPath(ring, 25.0, 25.0),
      "scaled Skia hit_test_path should accept logical window coordinates");
  Require<Backend>(!canvas.HitTestPath(ring, 50.0, 50.0),
      "scaled Skia hit_test_path should respect even-odd holes");
  Require<Backend>(!canvas.HitTestPath(ring, 90.0, 90.0),
      "scaled Skia hit_test_path should reject logical window misses");
}

void VerifyBlend2dWindowCoordinateHitTesting() {
  using Backend = luna::backend::blend2d::Backend;
  using Canvas = typename Backend::Canvas;
  using Path = typename Backend::Path;

  BLImage image(kWidth * 2, kHeight * 2, BL_FORMAT_PRGB32);
  if (image.is_empty()) {
    throw std::runtime_error("failed to create Blend2D image");
  }

  BLContext ctx(image);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0u));
  ctx.end();

  Canvas canvas;
  canvas.Init(std::move(image), luna::backend::blend2d::MakeRuntimeFontManager(),
      BLMatrix2D::make_scaling(2.0, 2.0));

  Require<Backend>(canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 25.0, 35.0),
      "scaled Blend2D hit_test_rect should accept logical window coordinates");
  Require<Backend>(!canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 5.0, 35.0),
      "scaled Blend2D hit_test_rect should reject logical window misses");

  canvas.Save();
  canvas.Translate(15.0, -5.0);
  Require<Backend>(canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 40.0, 30.0),
      "scaled Blend2D hit_test_rect should honor canvas transforms");
  Require<Backend>(!canvas.HitTestRect(10.0, 20.0, 30.0, 40.0, 20.0, 30.0),
      "scaled Blend2D hit_test_rect should miss transformed logical points");
  canvas.Restore();

  Path ring(EvenOddFillType<Backend>());
  ring.MoveTo(20.0, 20.0);
  ring.LineTo(80.0, 20.0);
  ring.LineTo(80.0, 80.0);
  ring.LineTo(20.0, 80.0);
  ring.Close();
  ring.MoveTo(35.0, 35.0);
  ring.LineTo(65.0, 35.0);
  ring.LineTo(65.0, 65.0);
  ring.LineTo(35.0, 65.0);
  ring.Close();

  Require<Backend>(canvas.HitTestPath(ring, 25.0, 25.0),
      "scaled Blend2D hit_test_path should accept logical window coordinates");
  Require<Backend>(!canvas.HitTestPath(ring, 50.0, 50.0),
      "scaled Blend2D hit_test_path should respect even-odd holes");
  Require<Backend>(!canvas.HitTestPath(ring, 90.0, 90.0),
      "scaled Blend2D hit_test_path should reject logical window misses");
}

template <typename Backend>
typename Backend::Path MakeRandomClipPolygon(Random *rng) {
  using Path = typename Backend::Path;

  const double cx = rng->Real(20.0, kWidth - 20.0);
  const double cy = rng->Real(20.0, kHeight - 20.0);
  const int points = rng->Int(3, 6);
  const double radius = rng->Real(10.0, 34.0);

  std::vector<double> angles;
  angles.reserve(static_cast<size_t>(points));
  for (int i = 0; i < points; ++i) {
    angles.push_back(rng->Real(0.0, 2.0 * 3.14159265358979323846));
  }
  std::sort(angles.begin(), angles.end());

  Path path;
  for (int i = 0; i < points; ++i) {
    const double r = radius * rng->Real(0.55, 1.0);
    const double x = cx + std::cos(angles[static_cast<size_t>(i)]) * r;
    const double y = cy + std::sin(angles[static_cast<size_t>(i)]) * r;
    if (i == 0) {
      path.MoveTo(x, y);
    } else {
      path.LineTo(x, y);
    }
  }
  path.Close();
  return path;
}

template <typename Backend>
void DrawRandomScene(
    typename Backend::Canvas *canvas, uint64_t seed, int ops_per_scene) {
  using Image = typename Backend::Image;
  using Paint = typename Backend::Paint;
  using Path = typename Backend::Path;

  Random rng(seed);
  Image pattern = MakeSnapshotPattern<Backend>();

  canvas->Clear(RandomColor(&rng));

  int depth = 1;
  constexpr int kMaxDepth = 6;

  for (int i = 0; i < ops_per_scene; ++i) {
    const int op = rng.Int(0, 13);
    switch (op) {
    case 0: {
      Paint paint = MakeRandomSolidPaint<Backend>(&rng, false);
      canvas->DrawRect(
          RandomX(&rng), RandomY(&rng), RandomW(&rng), RandomH(&rng), &paint);
      break;
    }
    case 1: {
      Paint paint = MakeRandomSolidPaint<Backend>(&rng, true);
      canvas->DrawRect(
          RandomX(&rng), RandomY(&rng), RandomW(&rng), RandomH(&rng), &paint);
      break;
    }
    case 2: {
      Paint paint = MakeRandomSolidPaint<Backend>(&rng, rng.Chance(0.5));
      canvas->DrawRoundRect(RandomX(&rng), RandomY(&rng), RandomW(&rng),
          RandomH(&rng), rng.Real(1.0, 14.0), rng.Real(1.0, 14.0), &paint);
      break;
    }
    case 3: {
      Paint paint = MakeRandomSolidPaint<Backend>(&rng, rng.Chance(0.35));
      Path path = MakeRandomPath<Backend>(&rng);
      canvas->DrawPath(path, &paint);
      break;
    }
    case 4: {
      Paint paint = MakeRandomLinearGradientPaint<Backend>(&rng);
      canvas->Save();
      ++depth;
      canvas->Translate(RandomX(&rng), RandomY(&rng));
      canvas->DrawRoundRect(rng.Real(-10.0, 10.0), rng.Real(-10.0, 10.0),
          RandomW(&rng), RandomH(&rng), rng.Real(2.0, 12.0),
          rng.Real(2.0, 12.0), &paint);
      canvas->Restore();
      --depth;
      break;
    }
    case 5: {
      Paint paint = MakeRandomRadialGradientPaint<Backend>(&rng);
      canvas->Save();
      ++depth;
      canvas->Translate(RandomX(&rng), RandomY(&rng));
      canvas->DrawRect(rng.Real(-18.0, 2.0), rng.Real(-18.0, 2.0),
          RandomW(&rng), RandomH(&rng), &paint);
      canvas->Restore();
      --depth;
      break;
    }
    case 6: {
      Paint paint = MakeImagePaint<Backend>(
          &pattern, luna::backend::ImageSamplingMode::kLinear);
      canvas->DrawRect(
          RandomX(&rng), RandomY(&rng), RandomW(&rng), RandomH(&rng), &paint);
      break;
    }
    case 7: {
      const bool aligned = rng.Chance(0.6);
      const double x =
          aligned ? double(rng.Int(-12, kWidth - 4)) : RandomX(&rng);
      const double y =
          aligned ? double(rng.Int(-12, kHeight - 4)) : RandomY(&rng);
      const double w = aligned ? double(rng.Int(4, 64)) : RandomW(&rng);
      const double h = aligned ? double(rng.Int(4, 56)) : RandomH(&rng);
      canvas->ClipRect(x, y, w, h);
      break;
    }
    case 8: {
      canvas->ClipRoundRect(RandomX(&rng), RandomY(&rng), RandomW(&rng),
          RandomH(&rng), rng.Real(2.0, 12.0), rng.Real(2.0, 12.0));
      break;
    }
    case 9: {
      Path clip = MakeRandomClipPolygon<Backend>(&rng);
      canvas->ClipPath(clip);
      break;
    }
    case 10: {
      if (depth < kMaxDepth) {
        canvas->Save();
        ++depth;
      } else {
        canvas->Translate(rng.Real(-16.0, 16.0), rng.Real(-16.0, 16.0));
      }
      break;
    }
    case 11: {
      if (depth < kMaxDepth) {
        Paint layer_paint;
        layer_paint.SetAlpha(rng.Real(0.35, 1.0));
        canvas->SaveLayer(&layer_paint);
        ++depth;
      }
      break;
    }
    case 12: {
      if (depth > 1) {
        canvas->Restore();
        --depth;
      }
      break;
    }
    case 13: {
      const int transform = rng.Int(0, 3);
      if (transform == 0) {
        canvas->Translate(rng.Real(-20.0, 20.0), rng.Real(-20.0, 20.0));
      } else if (transform == 1) {
        canvas->Rotate(rng.Real(-0.65, 0.65));
      } else if (transform == 2) {
        canvas->Scale(rng.Real(0.75, 1.25), rng.Real(0.75, 1.25));
      } else {
        canvas->Skew(rng.Real(-0.15, 0.15), rng.Real(-0.15, 0.15));
      }
      break;
    }
    default:
      break;
    }

    if (rng.Chance(0.04)) {
      canvas->Clear(RandomColor(&rng, 180, 255));
    }
  }

  while (depth > 1) {
    canvas->Restore();
    --depth;
  }
}

void WritePng(const PixelBuffer &buffer, const std::filesystem::path &path) {
  const SkImageInfo info = SkImageInfo::Make(
      buffer.width, buffer.height, kN32_SkColorType, kPremul_SkAlphaType);
  SkPixmap pixmap(
      info, buffer.bytes.data(), static_cast<size_t>(buffer.stride));
  SkFILEWStream stream(path.c_str());
  if (!stream.isValid()) {
    throw std::runtime_error(fmt::format("failed to open {}", path.string()));
  }
  if (!SkPngEncoder::Encode(&stream, pixmap, {})) {
    throw std::runtime_error(fmt::format("failed to encode {}", path.string()));
  }
}

PixelBuffer MakeDiffImage(const PixelBuffer &a, const PixelBuffer &b) {
  if (a.width != b.width || a.height != b.height) {
    throw std::runtime_error("diff image size mismatch");
  }

  PixelBuffer out;
  out.width = a.width;
  out.height = a.height;
  out.stride = a.width * 4;
  out.bytes.resize(
      static_cast<size_t>(out.stride) * static_cast<size_t>(out.height));

  for (int y = 0; y < a.height; ++y) {
    const uint8_t *row_a = a.bytes.data() + static_cast<size_t>(y) * a.stride;
    const uint8_t *row_b = b.bytes.data() + static_cast<size_t>(y) * b.stride;
    uint8_t *row_out = out.bytes.data() + static_cast<size_t>(y) * out.stride;
    for (int x = 0; x < a.width; ++x) {
      int pixel_max = 0;
      for (int c = 0; c < 4; ++c) {
        pixel_max = std::max(
            pixel_max, std::abs(int(row_a[x * 4 + c]) - int(row_b[x * 4 + c])));
      }

      row_out[x * 4 + 0] = static_cast<uint8_t>(std::min(pixel_max * 4, 255));
      row_out[x * 4 + 1] = 0;
      row_out[x * 4 + 2] = 0;
      row_out[x * 4 + 3] = 255;
    }
  }

  return out;
}

PixelBuffer Capture(luna::backend::skia::Canvas *canvas) {
  luna::backend::skia::Image image;
  std::string error;
  if (!canvas->Snapshot(&image, &error)) {
    throw std::runtime_error(error);
  }

  SkPixmap pixmap;
  if (!image.sk || !image.sk->peekPixels(&pixmap)) {
    throw std::runtime_error("failed to read Skia pixels");
  }

  PixelBuffer out;
  out.width = pixmap.width();
  out.height = pixmap.height();
  out.stride = static_cast<int>(pixmap.rowBytes());
  out.bytes.resize(
      static_cast<size_t>(out.stride) * static_cast<size_t>(out.height));
  std::memcpy(out.bytes.data(), pixmap.addr(), out.bytes.size());
  return out;
}

PixelBuffer Capture(luna::backend::blend2d::Canvas *canvas) {
  canvas->Flush();

  luna::backend::blend2d::Image image;
  std::string error;
  if (!canvas->Snapshot(&image, &error)) {
    throw std::runtime_error(error);
  }

  BLImageData data{};
  if (image.image.get_data(&data) != BL_SUCCESS || data.pixel_data == nullptr) {
    throw std::runtime_error("failed to read Blend2D pixels");
  }

  PixelBuffer out;
  out.width = data.size.w;
  out.height = data.size.h;
  out.stride = static_cast<int>(data.stride);
  out.bytes.resize(
      static_cast<size_t>(out.stride) * static_cast<size_t>(out.height));
  std::memcpy(out.bytes.data(), data.pixel_data, out.bytes.size());
  return out;
}

DiffSummary Compare(const PixelBuffer &a, const PixelBuffer &b) {
  if (a.width != b.width || a.height != b.height) {
    throw std::runtime_error(fmt::format("image size mismatch {}x{} vs {}x{}",
        a.width, a.height, b.width, b.height));
  }

  DiffSummary diff;
  bool saw_over_8 = false;
  for (int y = 0; y < a.height; ++y) {
    const uint8_t *row_a = a.bytes.data() + static_cast<size_t>(y) * a.stride;
    const uint8_t *row_b = b.bytes.data() + static_cast<size_t>(y) * b.stride;
    for (int x = 0; x < a.width; ++x) {
      int pixel_max = 0;
      for (int c = 0; c < 4; ++c) {
        const int delta =
            std::abs(int(row_a[x * 4 + c]) - int(row_b[x * 4 + c]));
        pixel_max = std::max(pixel_max, delta);
        if (delta > diff.max_channel_delta) {
          diff.max_channel_delta = delta;
          diff.max_x = x;
          diff.max_y = y;
        }
      }
      if (pixel_max > 2) {
        ++diff.pixels_over_2;
      }
      if (pixel_max > 8) {
        ++diff.pixels_over_8;
        if (!saw_over_8) {
          diff.over_8_min_x = x;
          diff.over_8_min_y = y;
          diff.over_8_max_x = x;
          diff.over_8_max_y = y;
          saw_over_8 = true;
        } else {
          diff.over_8_min_x = std::min(diff.over_8_min_x, x);
          diff.over_8_min_y = std::min(diff.over_8_min_y, y);
          diff.over_8_max_x = std::max(diff.over_8_max_x, x);
          diff.over_8_max_y = std::max(diff.over_8_max_y, y);
        }
      }
    }
  }
  return diff;
}

bool IsFailure(const PixelBuffer &pixels, const DiffSummary &diff) {
  const size_t pixel_count =
      static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height);
  const double over_2_ratio =
      pixel_count == 0 ? 0.0 : double(diff.pixels_over_2) / double(pixel_count);
  const double over_8_ratio =
      pixel_count == 0 ? 0.0 : double(diff.pixels_over_8) / double(pixel_count);
  return over_8_ratio > kAllowedErrorRatio || over_2_ratio > kAllowedErrorRatio;
}

FuzzConfig ParseArgs(int argc, char **argv) {
  FuzzConfig config;
  if (argc > 1) {
    config.iterations = std::max(1, std::atoi(argv[1]));
  }
  if (argc > 2) {
    config.base_seed = std::strtoull(argv[2], nullptr, 0);
  }
  if (argc > 3) {
    config.ops_per_scene = std::max(1, std::atoi(argv[3]));
  }
  return config;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const FuzzConfig config = ParseArgs(argc, argv);
    VerifyHitTesting<luna::backend::skia::Backend>();
    VerifySkiaWindowCoordinateHitTesting();
    VerifyHitTesting<luna::backend::blend2d::Backend>();
    VerifyBlend2dWindowCoordinateHitTesting();
    const std::filesystem::path dump_dir =
        std::filesystem::current_path() / "parity_dumps";
    std::filesystem::create_directories(dump_dir);

    std::vector<std::string> failures;
    int dumped_failures = 0;

    for (int i = 0; i < config.iterations; ++i) {
      const uint64_t seed = config.base_seed + static_cast<uint64_t>(i);

      auto skia_canvas =
          CanvasFactory<luna::backend::skia::Backend>::MakeCanvas(
              kWidth, kHeight);
      auto blend_canvas =
          CanvasFactory<luna::backend::blend2d::Backend>::MakeCanvas(
              kWidth, kHeight);

      DrawRandomScene<luna::backend::skia::Backend>(
          &skia_canvas, seed, config.ops_per_scene);
      DrawRandomScene<luna::backend::blend2d::Backend>(
          &blend_canvas, seed, config.ops_per_scene);

      const PixelBuffer skia_pixels = Capture(&skia_canvas);
      const PixelBuffer blend_pixels = Capture(&blend_canvas);
      const DiffSummary diff = Compare(skia_pixels, blend_pixels);
      if (!IsFailure(skia_pixels, diff)) {
        continue;
      }

      failures.push_back(
          fmt::format("seed=0x{:016x} iteration={} max_delta={} at ({}, {}) "
                      "pixels_over_8={} "
                      "bbox_over_8=[{},{}]-[{},{}] pixels_over_2={} ops={}",
              seed, i, diff.max_channel_delta, diff.max_x, diff.max_y,
              diff.pixels_over_8, diff.over_8_min_x, diff.over_8_min_y,
              diff.over_8_max_x, diff.over_8_max_y, diff.pixels_over_2,
              config.ops_per_scene));

      if (dumped_failures < kMaxFailuresToDump) {
        const PixelBuffer diff_pixels =
            MakeDiffImage(skia_pixels, blend_pixels);
        const std::string stem = fmt::format("fuzz_{:016x}", seed);
        WritePng(skia_pixels, dump_dir / fmt::format("{}_skia.png", stem));
        WritePng(blend_pixels, dump_dir / fmt::format("{}_blend2d.png", stem));
        WritePng(diff_pixels, dump_dir / fmt::format("{}_diff.png", stem));
        ++dumped_failures;
      }
    }

    if (!failures.empty()) {
      std::string message =
          fmt::format("render parity fuzz failed: iterations={} "
                      "base_seed=0x{:016x} ops={} failures={}",
              config.iterations, config.base_seed, config.ops_per_scene,
              failures.size());
      for (const std::string &failure : failures) {
        message += '\n';
        message += failure;
      }
      throw std::runtime_error(message);
    }

    fmt::print(
        "render parity fuzz passed: iterations={} base_seed=0x{:016x} ops={}\n",
        config.iterations, config.base_seed, config.ops_per_scene);
    return EXIT_SUCCESS;
  } catch (const std::exception &e) {
    fmt::print(stderr, "{}\n", e.what());
    return EXIT_FAILURE;
  }
}
