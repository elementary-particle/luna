#ifndef LUNA_CANVAS_PATH_H
#define LUNA_CANVAS_PATH_H

#include "lua_util.hpp"

namespace luna::backend {

template <typename B> class LPath {
private:
  using Canvas = typename B::Canvas;
  using Path = typename B::Path;
  using Paint = typename B::Paint;
  using FillType = typename B::FillType;

private:
  static int L_SetFillType(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    auto fill_type = static_cast<B::FillType>(luaL_checkinteger(L, 2));
    path->SetFillType(fill_type);
    lua_settop(L, 1);
    return 1;
  }

  static int L_MoveTo(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    double const x = static_cast<double>(luaL_checknumber(L, 2));
    double const y = static_cast<double>(luaL_checknumber(L, 3));
    path->MoveTo(x, y);
    lua_settop(L, 1);
    return 1;
  }

  static int L_LineTo(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    double const x = static_cast<double>(luaL_checknumber(L, 2));
    double const y = static_cast<double>(luaL_checknumber(L, 3));
    path->LineTo(x, y);
    lua_settop(L, 1);
    return 1;
  }

  static int L_QuadTo(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    double const x1 = static_cast<double>(luaL_checknumber(L, 2));
    double const y1 = static_cast<double>(luaL_checknumber(L, 3));
    double const x2 = static_cast<double>(luaL_checknumber(L, 4));
    double const y2 = static_cast<double>(luaL_checknumber(L, 5));
    path->QuadTo(x1, y1, x2, y2);
    lua_settop(L, 1);
    return 1;
  }

  static int L_SmoothQuadTo(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    double const x2 = static_cast<double>(luaL_checknumber(L, 2));
    double const y2 = static_cast<double>(luaL_checknumber(L, 3));
    path->SmoothQuadTo(x2, y2);
    lua_settop(L, 1);
    return 1;
  }

  static int L_CubicTo(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    double const x1 = static_cast<double>(luaL_checknumber(L, 2));
    double const y1 = static_cast<double>(luaL_checknumber(L, 3));
    double const x2 = static_cast<double>(luaL_checknumber(L, 4));
    double const y2 = static_cast<double>(luaL_checknumber(L, 5));
    double const x3 = static_cast<double>(luaL_checknumber(L, 6));
    double const y3 = static_cast<double>(luaL_checknumber(L, 7));
    path->CubicTo(x1, y1, x2, y2, x3, y3);
    lua_settop(L, 1);
    return 1;
  }

  static int L_SmoothCubicTo(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    double const x2 = static_cast<double>(luaL_checknumber(L, 2));
    double const y2 = static_cast<double>(luaL_checknumber(L, 3));
    double const x3 = static_cast<double>(luaL_checknumber(L, 4));
    double const y3 = static_cast<double>(luaL_checknumber(L, 5));
    path->SmoothCubicTo(x2, y2, x3, y3);
    lua_settop(L, 1);
    return 1;
  }

  static int L_ArcTo(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    double const rx = static_cast<double>(luaL_checknumber(L, 2));
    double const ry = static_cast<double>(luaL_checknumber(L, 3));
    double const x_axis_rotation = static_cast<double>(luaL_checknumber(L, 4));
    bool const large_arc_flag = lua_toboolean(L, 5) != 0;
    bool const sweep_flag = lua_toboolean(L, 6) != 0;
    double const x1 = static_cast<double>(luaL_checknumber(L, 7));
    double const y1 = static_cast<double>(luaL_checknumber(L, 8));
    path->ArcTo(rx, ry, x_axis_rotation, large_arc_flag, sweep_flag, x1, y1);
    lua_settop(L, 1);
    return 1;
  }

  static int L_Close(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    path->Close();
    lua_settop(L, 1);
    return 1;
  }

  static int L_Reset(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    path->Reset();
    lua_settop(L, 1);
    return 1;
  }

  static int L_ToSvgString(lua_State *L) {
    auto *path = lua::Check<Path>(L, 1);
    const bool relative = !lua_isnoneornil(L, 2) && (lua_toboolean(L, 2) != 0);
    const auto svg = path->ToSvgString(relative);
    lua_pushlstring(L, svg.data(), svg.size());
    return 1;
  }

public:
  static int L_New(lua_State *L) {
    lua::Check<Canvas>(L, 1);

    FillType fill_type = B::DefaultFillType();
    if (!lua_isnoneornil(L, 2)) {
      fill_type = static_cast<B::FillType>(luaL_checkinteger(L, 2));
    }

    lua::New<Path>(L, fill_type);
    return 1;
  }

  static int L_FromSvgString(lua_State *L) {
    lua::Check<Canvas>(L, 1);
    const char *svg = luaL_checkstring(L, 2);
    if (!svg || !*svg) {
      return luaL_error(L, "path: SVG path string must not be empty");
    }

    auto parsed = Path::FromSvgString(svg);
    if (!parsed.has_value()) {
      return luaL_error(L, "path: invalid SVG path string");
    }

    if (!lua_isnoneornil(L, 3)) {
      parsed->SetFillType(static_cast<B::FillType>(luaL_checkinteger(L, 3)));
    }

    lua::New<Path>(L, std::move(*parsed));
    return 1;
  }

public:
  static void Bind(lua_State *L) {
    if (lua::NewType<Path>(L)) {
      lua_pushcfunction(L, &L_MoveTo);
      lua_setfield(L, -2, "move_to");
      lua_pushcfunction(L, &L_LineTo);
      lua_setfield(L, -2, "line_to");
      lua_pushcfunction(L, &L_QuadTo);
      lua_setfield(L, -2, "quad_to");
      lua_pushcfunction(L, &L_CubicTo);
      lua_setfield(L, -2, "cubic_to");
      lua_pushcfunction(L, &L_ArcTo);
      lua_setfield(L, -2, "arc_to");
      lua_pushcfunction(L, &L_Close);
      lua_setfield(L, -2, "close");
      lua_pushcfunction(L, &L_Reset);
      lua_setfield(L, -2, "reset");
      lua_pushcfunction(L, &L_SetFillType);
      lua_setfield(L, -2, "_set_fill_type");
      if constexpr (B::SupportsSvgPathSerialization()) {
        lua_pushcfunction(L, &L_ToSvgString);
        lua_setfield(L, -2, "to_svg_string");
      }
    }
    lua_pop(L, 1);
  }
};

} // namespace luna::backend

#endif
