#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include <skia/core/SkStream.h>

#include "backend/canvas.h"
#include "blend2d/canvas.h"
#include "blend2d/font_manager.h"
#include "skia/canvas.h"
#include "skia/font_manager.h"

namespace {

constexpr double kParagraphWidth = 220.0;
constexpr double kEpsilon = 1e-6;
constexpr uint32_t kColor = 0xFF102030u;
constexpr const char *kFamily = "ABeeZee";
constexpr const char *kText = "Alpha beta\nGamma delta\nEpsilon";

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void CheckMetrics(const luna::backend::ParagraphMetrics &metrics,
    size_t expected_text_size) {
  Require(std::abs(metrics.width - kParagraphWidth) <= kEpsilon,
      "paragraph width did not preserve max width");
  Require(metrics.height > 0.0, "paragraph height must be positive");
  Require(metrics.line_count == metrics.lines.size(),
      "line_count must match detailed line metrics");
  Require(metrics.line_count == 3, "expected three explicit lines");
  Require(metrics.alphabetic_baseline > 0.0,
      "alphabetic baseline must be positive");
  Require(metrics.ideographic_baseline >= metrics.alphabetic_baseline,
      "ideographic baseline must not precede alphabetic baseline");
  Require(metrics.max_intrinsic_width >= metrics.min_intrinsic_width,
      "max intrinsic width must be >= min intrinsic width");

  double previous_baseline = -1.0;
  for (const auto &line : metrics.lines) {
    Require(line.start_index <= line.end_index,
        "line start_index must be <= end_index");
    Require(line.end_index <= expected_text_size,
        "line end_index must stay within the source text");
    Require(line.width >= 0.0, "line width must be non-negative");
    Require(line.height > 0.0, "line height must be positive");
    Require(line.left >= -kEpsilon, "line left must be non-negative");
    Require(line.left + line.width <= metrics.width + 1.0,
        "line left + width must stay within paragraph width");
    Require(line.baseline >= previous_baseline,
        "line baselines must be monotonic");
    previous_baseline = line.baseline;
  }
}

luna::backend::ParagraphMetrics MeasureSkiaParagraph(
    const std::filesystem::path &font_path) {
  luna::backend::skia::Canvas canvas;
  canvas.set_font_manager(luna::backend::skia::MakeRuntimeFontManager());
  Require(luna::backend::skia::RegisterRuntimeFont(
              canvas.font_mgr(), SkStream::MakeFromFile(font_path.c_str())),
      "failed to register test font for skia");

  luna::backend::skia::Canvas::Font font;
  font.SetSize(24.0);
  font.SetFamily(kFamily);

  luna::backend::skia::Paragraph paragraph;
  std::string error;
  Require(paragraph.Init(canvas, font, kColor,
              luna::backend::skia::TextAlign::kCenter, std::nullopt, nullptr,
              &error),
      error.empty() ? "failed to initialize skia paragraph" : error);
  paragraph.AddText(kText);
  paragraph.Layout(kParagraphWidth);
  Require(paragraph.IsValid(),
      paragraph.Error().empty() ? "skia paragraph layout failed"
                                : paragraph.Error());
  return paragraph.Measure();
}

luna::backend::ParagraphMetrics MeasureBlend2dParagraph(
    const std::filesystem::path &font_path) {
  luna::backend::blend2d::Canvas canvas;
  canvas.font_mgr = luna::backend::blend2d::MakeRuntimeFontManager();
  Require(luna::backend::blend2d::RegisterRuntimeFont(
              &canvas.font_mgr, font_path.c_str()),
      "failed to register test font for blend2d");

  luna::backend::blend2d::Font font;
  font.SetSize(24.0);
  font.SetFamily(kFamily);

  luna::backend::blend2d::Paragraph paragraph;
  std::string error;
  Require(paragraph.Init(canvas, font, kColor,
              luna::backend::blend2d::TextAlign::kCenter, std::nullopt,
              nullptr, &error),
      error.empty() ? "failed to initialize blend2d paragraph" : error);
  paragraph.AddText(kText);
  paragraph.Layout(kParagraphWidth);
  Require(paragraph.IsValid(),
      paragraph.Error().empty() ? "blend2d paragraph layout failed"
                                : paragraph.Error());
  return paragraph.Measure();
}

} // namespace

int main() {
  const std::filesystem::path font_path =
      std::filesystem::path(LUNA_SOURCE_DIR) / "test" / "assets" /
      "ABeeZee-Regular.ttf";
  Require(std::filesystem::exists(font_path), "test font is missing");

  const luna::backend::ParagraphMetrics skia_metrics =
      MeasureSkiaParagraph(font_path);
  const luna::backend::ParagraphMetrics blend2d_metrics =
      MeasureBlend2dParagraph(font_path);

  CheckMetrics(skia_metrics, std::string(kText).size());
  CheckMetrics(blend2d_metrics, std::string(kText).size());

  Require(skia_metrics.line_count == blend2d_metrics.line_count,
      "backends must agree on explicit line count");
  for (size_t i = 0; i < skia_metrics.lines.size(); ++i) {
    Require(skia_metrics.lines[i].start_index ==
            blend2d_metrics.lines[i].start_index,
        "backends must agree on line start_index");
    Require(
        skia_metrics.lines[i].end_index == blend2d_metrics.lines[i].end_index,
        "backends must agree on line end_index");
  }

  return 0;
}
