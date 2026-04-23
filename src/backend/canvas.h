#ifndef LUNA_CANVAS_H
#define LUNA_CANVAS_H

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "backend/enums.h"
#include "backend/path.h"
#include "lua_util.hpp"

namespace luna::backend {

extern const char *CanvasHelper;

struct TextMetrics {
  double advance_width = 0;
  double bounds_x = 0;
  double bounds_y = 0;
  double bounds_w = 0;
  double bounds_h = 0;
  double ascent = 0;
  double descent = 0;
  double leading = 0;
  double line_height = 0;
};

struct ParagraphLineMetrics {
  size_t start_index = 0;
  size_t end_index = 0;
  double ascent = 0;
  double descent = 0;
  double height = 0;
  double width = 0;
  double left = 0;
  double baseline = 0;
};

struct ParagraphMetrics {
  double width = 0;
  double height = 0;
  double longest_line = 0;
  double min_intrinsic_width = 0;
  double max_intrinsic_width = 0;
  double alphabetic_baseline = 0;
  double ideographic_baseline = 0;
  bool did_exceed_max_lines = false;
  size_t line_count = 0;
  double line_height = 0;
  std::vector<ParagraphLineMetrics> lines;
};

template <typename B> class LCanvas {
public:
  static void Bind(lua_State *L) {
    BindSupportTypes(L);
    if (lua::NewType<Canvas>(L)) {
      BindStateMethods(L);
      BindFactoryMethods(L);
      BindDrawMethods(L);
      BindHitTestMethods(L);
      BindClipMethods(L);
      BindShaderMethods(L);
      BindParagraphMethods(L);
      LoadLuaHelpers(L);
    }
    lua_pop(L, 1);
  }

private:
  using Canvas = typename B::Canvas;
  using Image = typename B::Image;
  using Path = typename B::Path;
  using Paint = typename B::Paint;
  using Font = typename B::Font;
  using Shader = typename B::Shader;
  using Paragraph = typename B::Paragraph;
  using BlendMode = typename B::BlendMode;
  using ImageSamplingMode = typename B::ImageSamplingMode;
  using TileMode = typename B::TileMode;
  using TextAlign = typename B::TextAlign;

  static int L_ParagraphMeasure(lua_State *L) {
    auto *paragraph = lua::Check<Paragraph>(L, 1);
    if (!paragraph->IsValid()) {
      return luaL_error(L, "measure: paragraph is empty");
    }
    PushParagraphMetricsTable(L, paragraph->Measure());
    return 1;
  }

  static int L_Clear(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    canvas->Clear(static_cast<uint32_t>(luaL_checkinteger(L, 2)));
    return 0;
  }

  static int L_Save(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    canvas->Save();
    return 0;
  }

  static int L_SaveLayer(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    Paint *paint = nullptr;
    if (!lua_isnoneornil(L, 2)) {
      paint = lua::Check<Paint>(L, 2);
    }
    canvas->SaveLayer(paint);
    return 0;
  }

  static int L_Restore(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    canvas->Restore();
    return 0;
  }

  static int L_Translate(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    canvas->Translate(static_cast<double>(luaL_checknumber(L, 2)),
        static_cast<double>(luaL_checknumber(L, 3)));
    return 0;
  }

  static int L_Scale(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    canvas->Scale(static_cast<double>(luaL_checknumber(L, 2)),
        static_cast<double>(luaL_checknumber(L, 3)));
    return 0;
  }

  static int L_Skew(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    canvas->Skew(static_cast<double>(luaL_checknumber(L, 2)),
        static_cast<double>(luaL_checknumber(L, 3)));
    return 0;
  }

  static int L_Rotate(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    canvas->Rotate(static_cast<double>(luaL_checknumber(L, 2)));
    return 0;
  }

  static int L_Snapshot(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    auto *image = lua::New<Image>(L);
    std::string error;
    if (!canvas->Snapshot(image, &error)) {
      return luaL_error(L, "%s", error.c_str());
    }
    return 1;
  }

  static int L_Paint(lua_State *L) {
    lua::Check<Canvas>(L, 1);

    auto *paint = lua::New<Paint>(L);
    if (!lua_isnoneornil(L, 2)) {
      auto *shader = lua::Check<Shader>(L, 2);
      paint->SetShader(*shader);
    }
    if (!lua_isnoneornil(L, 3)) {
      paint->SetAlpha(static_cast<double>(luaL_checknumber(L, 3)));
    }
    if (!lua_isnoneornil(L, 4)) {
      paint->SetBlendMode(static_cast<BlendMode>(luaL_checkinteger(L, 4)));
    }
    if (!lua_isnoneornil(L, 5)) {
      auto style = static_cast<PaintStyle>(luaL_checkinteger(L, 5));
      switch (style) {
      case PaintStyle::kFill: {
        paint->SetFillStyle();
        break;
      }
      case PaintStyle::kStroke: {
        double stroke_width = static_cast<double>(luaL_checknumber(L, 6));
        paint->SetStrokeStyle(stroke_width);
        break;
      }
      default:
        return luaL_error(L, "unknown paint style %d", style);
      }
    }
    return 1;
  }

  static int L_Font(lua_State *L) {
    lua::Check<Canvas>(L, 1);

    auto *font = lua::New<Font>(L);
    font->SetSize(static_cast<double>(luaL_checknumber(L, 2)));
    if (!lua_isnoneornil(L, 3)) {
      const char *family = luaL_checkstring(L, 3);
      if (!family || !*family) {
        return luaL_error(L, "font: family must not be empty");
      }
      font->SetFamily(family);
    }
    const int weight = lua_isnoneornil(L, 4)
        ? B::DefaultFontWeight()
        : static_cast<int>(luaL_checkinteger(L, 4));
    const int width = lua_isnoneornil(L, 5)
        ? B::DefaultFontWidth()
        : static_cast<int>(luaL_checkinteger(L, 5));
    const int slant = lua_isnoneornil(L, 6)
        ? B::DefaultFontSlant()
        : static_cast<int>(luaL_checkinteger(L, 6));
    font->SetStyle(weight, width, slant);
    return 1;
  }

  static int L_DrawRect(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    Paint *paint = nullptr;
    if (!lua_isnoneornil(L, 6)) {
      paint = lua::Check<Paint>(L, 6);
    }
    canvas->DrawRect(static_cast<double>(luaL_checknumber(L, 2)),
        static_cast<double>(luaL_checknumber(L, 3)),
        static_cast<double>(luaL_checknumber(L, 4)),
        static_cast<double>(luaL_checknumber(L, 5)), paint);
    return 0;
  }

  static int L_DrawRoundRect(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    Paint *paint = nullptr;
    if (!lua_isnoneornil(L, 8)) {
      paint = lua::Check<Paint>(L, 8);
    }
    canvas->DrawRoundRect(static_cast<double>(luaL_checknumber(L, 2)),
        static_cast<double>(luaL_checknumber(L, 3)),
        static_cast<double>(luaL_checknumber(L, 4)),
        static_cast<double>(luaL_checknumber(L, 5)),
        static_cast<double>(luaL_checknumber(L, 6)),
        static_cast<double>(luaL_checknumber(L, 7)), paint);
    return 0;
  }

  static int L_DrawPath(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    auto *path = lua::Check<Path>(L, 2);

    Paint *paint = nullptr;
    if (!lua_isnoneornil(L, 3)) {
      paint = lua::Check<Paint>(L, 3);
    }

    canvas->DrawPath(*path, paint);
    return 0;
  }

  static int L_HitTestRect(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    const bool hit =
        canvas->HitTestRect(static_cast<double>(luaL_checknumber(L, 2)),
            static_cast<double>(luaL_checknumber(L, 3)),
            static_cast<double>(luaL_checknumber(L, 4)),
            static_cast<double>(luaL_checknumber(L, 5)),
            static_cast<double>(luaL_checknumber(L, 6)),
            static_cast<double>(luaL_checknumber(L, 7)));
    lua_pushboolean(L, hit);
    return 1;
  }

  static int L_HitTestPath(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    auto *path = lua::Check<Path>(L, 2);
    const bool hit =
        canvas->HitTestPath(*path, static_cast<double>(luaL_checknumber(L, 3)),
            static_cast<double>(luaL_checknumber(L, 4)));
    lua_pushboolean(L, hit);
    return 1;
  }

  static int L_ClipRect(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);

    canvas->ClipRect(static_cast<double>(luaL_checknumber(L, 2)),
        static_cast<double>(luaL_checknumber(L, 3)),
        static_cast<double>(luaL_checknumber(L, 4)),
        static_cast<double>(luaL_checknumber(L, 5)));
    return 0;
  }

  static int L_ClipRoundRect(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);

    canvas->ClipRoundRect(static_cast<double>(luaL_checknumber(L, 2)),
        static_cast<double>(luaL_checknumber(L, 3)),
        static_cast<double>(luaL_checknumber(L, 4)),
        static_cast<double>(luaL_checknumber(L, 5)),
        static_cast<double>(luaL_checknumber(L, 6)),
        static_cast<double>(luaL_checknumber(L, 7)));
    return 0;
  }

  static int L_ClipPath(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    auto *path = lua::Check<Path>(L, 2);

    canvas->ClipPath(*path);
    return 0;
  }

  static int L_DrawText(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    size_t text_len = 0;
    const char *text = luaL_checklstring(L, 2, &text_len);
    auto *font = lua::Check<Font>(L, 5);

    Paint *paint = nullptr;
    if (!lua_isnoneornil(L, 6)) {
      paint = lua::Check<Paint>(L, 6);
    }

    std::string error;
    if (!canvas->DrawText(std::string_view(text, text_len),
            static_cast<double>(luaL_checknumber(L, 3)),
            static_cast<double>(luaL_checknumber(L, 4)), *font, paint,
            &error)) {
      return luaL_error(L, "%s", error.c_str());
    }
    return 0;
  }

  static int L_MeasureText(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    size_t text_len = 0;
    const char *text = luaL_checklstring(L, 2, &text_len);
    auto *font = lua::Check<Font>(L, 3);

    Paint *paint = nullptr;
    if (!lua_isnoneornil(L, 4)) {
      paint = lua::Check<Paint>(L, 4);
    }

    TextMetrics metrics;
    std::string error;
    if (!canvas->MeasureText(
            std::string_view(text, text_len), *font, paint, &metrics, &error)) {
      return luaL_error(L, "%s", error.c_str());
    }
    PushTextMetricsTable(L, metrics);
    return 1;
  }

  static int L_LinearGradient(lua_State *L) {
    lua::Check<Canvas>(L, 1);
    luaL_checktype(L, 6, LUA_TTABLE);

    std::vector<uint32_t> colors;
    if (!ReadColorArray(L, 6, &colors)) {
      return luaL_error(
          L, "shader.linear_gradient.colors must have at least 2 colors");
    }

    std::vector<float> positions;
    std::span<const float> position_span;
    if (!lua_isnoneornil(L, 7)) {
      luaL_checktype(L, 7, LUA_TTABLE);
      if (!ReadScalarArray(
              L, 7, static_cast<lua_Integer>(colors.size()), &positions)) {
        return luaL_error(
            L, "shader.linear_gradient.positions must match colors length");
      }
      position_span = positions;
    }

    TileMode tile_mode = B::DefaultTileMode();
    if (!lua_isnoneornil(L, 8)) {
      tile_mode = static_cast<B::TileMode>(luaL_checkinteger(L, 8));
    }

    auto *shader = lua::New<Shader>(L);
    std::string error;
    if (!shader->MakeLinearGradient(static_cast<double>(luaL_checknumber(L, 2)),
            static_cast<double>(luaL_checknumber(L, 3)),
            static_cast<double>(luaL_checknumber(L, 4)),
            static_cast<double>(luaL_checknumber(L, 5)), colors, position_span,
            tile_mode, &error)) {
      return luaL_error(L, "%s", error.c_str());
    }
    return 1;
  }

  static int L_RadialGradient(lua_State *L) {
    lua::Check<Canvas>(L, 1);
    luaL_checktype(L, 5, LUA_TTABLE);

    std::vector<uint32_t> colors;
    if (!ReadColorArray(L, 5, &colors)) {
      return luaL_error(
          L, "shader.radial_gradient.colors must have at least 2 colors");
    }

    std::vector<float> positions;
    std::span<const float> position_span;
    if (!lua_isnoneornil(L, 6)) {
      luaL_checktype(L, 6, LUA_TTABLE);
      if (!ReadScalarArray(
              L, 6, static_cast<lua_Integer>(colors.size()), &positions)) {
        return luaL_error(
            L, "shader.radial_gradient.positions must match colors length");
      }
      position_span = positions;
    }

    TileMode tile_mode = B::DefaultTileMode();
    if (!lua_isnoneornil(L, 7)) {
      tile_mode = static_cast<B::TileMode>(luaL_checkinteger(L, 7));
    }

    auto *shader = lua::New<Shader>(L);
    std::string error;
    if (!shader->MakeRadialGradient(static_cast<double>(luaL_checknumber(L, 2)),
            static_cast<double>(luaL_checknumber(L, 3)),
            static_cast<double>(luaL_checknumber(L, 4)), colors, position_span,
            tile_mode, &error)) {
      return luaL_error(L, "%s", error.c_str());
    }
    return 1;
  }

  static int L_SolidColor(lua_State *L) {
    lua::Check<Canvas>(L, 1);

    auto *shader = lua::New<Shader>(L);
    shader->MakeSolidColor(static_cast<uint32_t>(luaL_checkinteger(L, 2)));
    return 1;
  }

  static int L_ImageShader(lua_State *L) {
    lua::Check<Canvas>(L, 1);
    auto *image = lua::Check<Image>(L, 2);

    ImageSamplingMode sampling = B::DefaultImageSamplingMode();
    if (!lua_isnoneornil(L, 3)) {
      sampling = static_cast<B::ImageSamplingMode>(luaL_checkinteger(L, 3));
    }

    TileMode tile_mode = B::DefaultTileMode();
    if (!lua_isnoneornil(L, 4)) {
      tile_mode = static_cast<B::TileMode>(luaL_checkinteger(L, 4));
    }

    auto *shader = lua::New<Shader>(L);
    std::string error;
    shader->MakeImage(image, sampling, tile_mode, &error);
    if (!error.empty()) {
      return luaL_error(L, "%s", error.c_str());
    }
    return 1;
  }

  static int L_Paragraph(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    const double width = static_cast<double>(luaL_checknumber(L, 2));
    if (!(width > 0.0)) {
      return luaL_error(L, "paragraph.width must be positive");
    }
    luaL_checktype(L, 3, LUA_TTABLE);

    TextAlign align = B::DefaultTextAlign();
    if (!lua_isnoneornil(L, 4)) {
      align = static_cast<B::TextAlign>(luaL_checkinteger(L, 4));
    }

    std::optional<size_t> max_lines;
    if (!lua_isnoneornil(L, 5)) {
      const lua_Integer lines = luaL_checkinteger(L, 5);
      if (lines <= 0) {
        return luaL_error(L, "paragraph.max_lines must be positive");
      }
      max_lines = static_cast<size_t>(lines);
    }

    const char *ellipsis = nullptr;
    if (!lua_isnoneornil(L, 6)) {
      ellipsis = luaL_checkstring(L, 6);
    }

    const lua_Integer segment_count =
        static_cast<lua_Integer>(lua_objlen(L, 3));
    if (segment_count <= 0) {
      return luaL_error(
          L, "paragraph.segments must contain at least 1 segment");
    }

    Font default_font;
    default_font.SetSize(14.0);
    uint32_t default_color = 0xFFFFFFFFu;
    bool found_default_font = false;
    bool found_default_color = false;

    for (lua_Integer i = 1; i <= segment_count; ++i) {
      lua_rawgeti(L, 3, i);
      luaL_checktype(L, -1, LUA_TTABLE);

      lua_getfield(L, -1, "font");
      if (!lua_isnil(L, -1)) {
        default_font = *lua::Check<Font>(L, -1);
        found_default_font = true;
      }
      lua_pop(L, 1);

      lua_getfield(L, -1, "color");
      if (!lua_isnil(L, -1)) {
        default_color = static_cast<uint32_t>(luaL_checkinteger(L, -1));
        found_default_color = true;
      }
      lua_pop(L, 2);

      if (found_default_font && found_default_color) {
        break;
      }
    }

    auto *paragraph = lua::New<Paragraph>(L);
    std::string error;
    if (!paragraph->Init(*canvas, default_font, default_color, align, max_lines,
            ellipsis, &error)) {
      return luaL_error(L, "%s", error.c_str());
    }

    for (lua_Integer i = 1; i <= segment_count; ++i) {
      lua_rawgeti(L, 3, i);
      luaL_checktype(L, -1, LUA_TTABLE);

      lua_getfield(L, -1, "text");
      size_t text_len = 0;
      const char *text = luaL_checklstring(L, -1, &text_len);
      lua_pop(L, 1);

      Font const *font = &default_font;
      lua_getfield(L, -1, "font");
      if (!lua_isnil(L, -1)) {
        font = lua::Check<Font>(L, -1);
      }
      lua_pop(L, 1);

      uint32_t color = default_color;
      lua_getfield(L, -1, "color");
      if (!lua_isnil(L, -1)) {
        color = static_cast<uint32_t>(luaL_checkinteger(L, -1));
      }
      lua_pop(L, 1);

      paragraph->PushStyle(*font, color);
      paragraph->AddText(std::string_view(text, text_len));
      paragraph->PopStyle();

      lua_pop(L, 1);
    }

    paragraph->Layout(width);
    if (!paragraph->IsValid()) {
      const std::string &paragraph_error = paragraph->Error();
      return luaL_error(L, "%s",
          paragraph_error.empty() ? "paragraph: failed to build paragraph"
                                  : paragraph_error.c_str());
    }
    return 1;
  }

  static int L_DrawParagraph(lua_State *L) {
    auto *canvas = lua::Check<Canvas>(L, 1);
    auto *paragraph = lua::Check<Paragraph>(L, 2);
    if (!paragraph->IsValid()) {
      return luaL_error(L, "draw_paragraph: paragraph is empty");
    }
    canvas->DrawParagraph(*paragraph,
        static_cast<double>(luaL_checknumber(L, 3)),
        static_cast<double>(luaL_checknumber(L, 4)));
    return 0;
  }

  static void BindSupportTypes(lua_State *L) {
    lua::NewType<Paint>(L);
    lua_pop(L, 1);
    lua::NewType<Font>(L);
    lua_pop(L, 1);
    lua::NewType<Shader>(L);
    lua_pop(L, 1);

    if (lua::NewType<Paragraph>(L)) {
      lua_pushcfunction(L, &L_ParagraphMeasure);
      lua_setfield(L, -2, "measure");
    }
    lua_pop(L, 1);

    LPath<B>::Bind(L);
  }

  static void BindStateMethods(lua_State *L) {
    lua_pushcfunction(L, &L_Clear);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, &L_Save);
    lua_setfield(L, -2, "save");
    lua_pushcfunction(L, &L_SaveLayer);
    lua_setfield(L, -2, "_save_layer");
    lua_pushcfunction(L, &L_Restore);
    lua_setfield(L, -2, "restore");
    lua_pushcfunction(L, &L_Translate);
    lua_setfield(L, -2, "translate");
    lua_pushcfunction(L, &L_Scale);
    lua_setfield(L, -2, "scale");
    lua_pushcfunction(L, &L_Skew);
    lua_setfield(L, -2, "skew");
    lua_pushcfunction(L, &L_Rotate);
    lua_setfield(L, -2, "rotate");
    lua_pushcfunction(L, &L_Snapshot);
    lua_setfield(L, -2, "snapshot");
  }

  static void BindFactoryMethods(lua_State *L) {
    lua_newtable(L);
    B::SetEnums(L);
    lua_setfield(L, -2, "_constants");

    lua_pushcfunction(L, &LPath<B>::L_New);
    lua_setfield(L, -2, "_path");
    if constexpr (B::SupportsSvgPathParsing()) {
      lua_pushcfunction(L, &LPath<B>::L_FromSvgString);
      lua_setfield(L, -2, "_path_svg");
    }
    lua_pushcfunction(L, &L_Paint);
    lua_setfield(L, -2, "_paint");
    lua_pushcfunction(L, &L_Font);
    lua_setfield(L, -2, "_font");
  }

  static void BindDrawMethods(lua_State *L) {
    lua_pushcfunction(L, &L_DrawRect);
    lua_setfield(L, -2, "_draw_rect");
    lua_pushcfunction(L, &L_DrawRoundRect);
    lua_setfield(L, -2, "_draw_round_rect");
    lua_pushcfunction(L, &L_DrawPath);
    lua_setfield(L, -2, "_draw_path");
    lua_pushcfunction(L, &L_DrawText);
    lua_setfield(L, -2, "_draw_text");
    lua_pushcfunction(L, &L_MeasureText);
    lua_setfield(L, -2, "_measure_text");
  }

  static void BindHitTestMethods(lua_State *L) {
    lua_pushcfunction(L, &L_HitTestRect);
    lua_setfield(L, -2, "_hit_test_rect");
    lua_pushcfunction(L, &L_HitTestPath);
    lua_setfield(L, -2, "_hit_test_path");
  }

  static void BindClipMethods(lua_State *L) {
    lua_pushcfunction(L, &L_ClipRect);
    lua_setfield(L, -2, "_clip_rect");
    lua_pushcfunction(L, &L_ClipRoundRect);
    lua_setfield(L, -2, "_clip_round_rect");
    lua_pushcfunction(L, &L_ClipPath);
    lua_setfield(L, -2, "_clip_path");
  }

  static void BindShaderMethods(lua_State *L) {
    lua_pushcfunction(L, &L_SolidColor);
    lua_setfield(L, -2, "_shader_solid_color");
    lua_pushcfunction(L, &L_LinearGradient);
    lua_setfield(L, -2, "_shader_linear_gradient");
    lua_pushcfunction(L, &L_RadialGradient);
    lua_setfield(L, -2, "_shader_radial_gradient");
    lua_pushcfunction(L, &L_ImageShader);
    lua_setfield(L, -2, "_shader_image");
  }

  static void BindParagraphMethods(lua_State *L) {
    lua_pushcfunction(L, &L_Paragraph);
    lua_setfield(L, -2, "_paragraph");
    lua_pushcfunction(L, &L_DrawParagraph);
    lua_setfield(L, -2, "_draw_paragraph");
  }

  static bool ReadColorArray(
      lua_State *L, int idx, std::vector<uint32_t> *colors) {
    idx = LuaAbsIndex(L, idx);
    const lua_Integer len = static_cast<lua_Integer>(lua_objlen(L, idx));
    if (len < 2) {
      return false;
    }

    colors->clear();
    colors->reserve(static_cast<size_t>(len));
    for (lua_Integer i = 1; i <= len; ++i) {
      lua_rawgeti(L, idx, i);
      colors->push_back(static_cast<uint32_t>(luaL_checkinteger(L, -1)));
      lua_pop(L, 1);
    }
    return true;
  }

  static bool ReadScalarArray(
      lua_State *L, int idx, lua_Integer expected, std::vector<float> *values) {
    idx = LuaAbsIndex(L, idx);
    const lua_Integer len = static_cast<lua_Integer>(lua_objlen(L, idx));
    if (len != expected) {
      return false;
    }

    values->clear();
    values->reserve(static_cast<size_t>(len));
    for (lua_Integer i = 1; i <= len; ++i) {
      lua_rawgeti(L, idx, i);
      values->push_back(static_cast<float>(luaL_checknumber(L, -1)));
      lua_pop(L, 1);
    }
    return true;
  }

  static int LuaAbsIndex(lua_State *L, int idx) {
    if (idx > 0 || idx <= LUA_REGISTRYINDEX) {
      return idx;
    }
    return lua_gettop(L) + idx + 1;
  }

  static void PushTextMetricsTable(lua_State *L, const TextMetrics &metrics) {
    lua_createtable(L, 0, 9);
    lua_pushnumber(L, metrics.advance_width);
    lua_setfield(L, -2, "advance_width");
    lua_pushnumber(L, metrics.bounds_x);
    lua_setfield(L, -2, "bounds_x");
    lua_pushnumber(L, metrics.bounds_y);
    lua_setfield(L, -2, "bounds_y");
    lua_pushnumber(L, metrics.bounds_w);
    lua_setfield(L, -2, "bounds_w");
    lua_pushnumber(L, metrics.bounds_h);
    lua_setfield(L, -2, "bounds_h");
    lua_pushnumber(L, metrics.ascent);
    lua_setfield(L, -2, "ascent");
    lua_pushnumber(L, metrics.descent);
    lua_setfield(L, -2, "descent");
    lua_pushnumber(L, metrics.leading);
    lua_setfield(L, -2, "leading");
    lua_pushnumber(L, metrics.line_height);
    lua_setfield(L, -2, "line_height");
  }

  static void PushParagraphMetricsTable(
      lua_State *L, const ParagraphMetrics &metrics) {
    lua_createtable(L, 0, 11);
    lua_pushnumber(L, metrics.width);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, metrics.height);
    lua_setfield(L, -2, "height");
    lua_pushnumber(L, metrics.longest_line);
    lua_setfield(L, -2, "longest_line");
    lua_pushnumber(L, metrics.min_intrinsic_width);
    lua_setfield(L, -2, "min_intrinsic_width");
    lua_pushnumber(L, metrics.max_intrinsic_width);
    lua_setfield(L, -2, "max_intrinsic_width");
    lua_pushnumber(L, metrics.alphabetic_baseline);
    lua_setfield(L, -2, "alphabetic_baseline");
    lua_pushnumber(L, metrics.ideographic_baseline);
    lua_setfield(L, -2, "ideographic_baseline");
    lua_pushboolean(L, metrics.did_exceed_max_lines);
    lua_setfield(L, -2, "did_exceed_max_lines");
    lua_pushinteger(L, static_cast<lua_Integer>(metrics.line_count));
    lua_setfield(L, -2, "line_count");
    lua_pushnumber(L, metrics.line_height);
    lua_setfield(L, -2, "line_height");
    lua_createtable(L, static_cast<int>(metrics.lines.size()), 0);
    for (size_t i = 0; i < metrics.lines.size(); ++i) {
      const ParagraphLineMetrics &line = metrics.lines[i];
      lua_createtable(L, 0, 8);
      lua_pushinteger(L, static_cast<lua_Integer>(line.start_index));
      lua_setfield(L, -2, "start_index");
      lua_pushinteger(L, static_cast<lua_Integer>(line.end_index));
      lua_setfield(L, -2, "end_index");
      lua_pushnumber(L, line.ascent);
      lua_setfield(L, -2, "ascent");
      lua_pushnumber(L, line.descent);
      lua_setfield(L, -2, "descent");
      lua_pushnumber(L, line.height);
      lua_setfield(L, -2, "height");
      lua_pushnumber(L, line.width);
      lua_setfield(L, -2, "width");
      lua_pushnumber(L, line.left);
      lua_setfield(L, -2, "left");
      lua_pushnumber(L, line.baseline);
      lua_setfield(L, -2, "baseline");
      lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "lines");
  }

  static void LoadLuaHelpers(lua_State *L) {
    if (luaL_loadbuffer(L, CanvasHelper, std::strlen(CanvasHelper),
            "canvas_path_helpers") != 0) {
      lua_error(L);
    }

    if (lua_pcall(L, 0, 1, 0) != 0) {
      lua_error(L);
    }

    lua_pushvalue(L, -2);
    luaL_getmetatable(L, B::Path::MT);
    if (lua_pcall(L, 2, 0, 0) != 0) {
      lua_error(L);
    }
  }
};

} // namespace luna::backend

#endif
