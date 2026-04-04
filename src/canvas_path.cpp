#include "canvas.h"

#include <cstring>
#include <optional>

#include <skia/core/SkPath.h>
#include <skia/core/SkPathBuilder.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkRRect.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTypes.h>
#include <skia/utils/SkParsePath.h>

namespace luna::canvas {

namespace {

SkPathFillType CheckPathFillType(lua_State *L, int idx) {
  return static_cast<SkPathFillType>(luaL_checkinteger(L, idx));
}

SkClipOp CheckClipOp(lua_State *L, int idx) {
  return static_cast<SkClipOp>(luaL_checkinteger(L, idx));
}

SkPathDirection CheckPathDirection(lua_State *L, int idx) {
  return static_cast<SkPathDirection>(luaL_checkinteger(L, idx));
}

} // namespace

void RegisterPathBindings(lua_State *L) {
  if (lua::NewType<LPath>(L)) {
    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      path->sk.moveTo(SkFloatToScalar(x), SkFloatToScalar(y));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "move_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      path->sk.lineTo(SkFloatToScalar(x), SkFloatToScalar(y));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "line_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x1 = static_cast<float>(luaL_checknumber(L, 2));
      const float y1 = static_cast<float>(luaL_checknumber(L, 3));
      const float x2 = static_cast<float>(luaL_checknumber(L, 4));
      const float y2 = static_cast<float>(luaL_checknumber(L, 5));
      path->sk.quadTo(SkFloatToScalar(x1),
                      SkFloatToScalar(y1),
                      SkFloatToScalar(x2),
                      SkFloatToScalar(y2));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "quad_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x1 = static_cast<float>(luaL_checknumber(L, 2));
      const float y1 = static_cast<float>(luaL_checknumber(L, 3));
      const float x2 = static_cast<float>(luaL_checknumber(L, 4));
      const float y2 = static_cast<float>(luaL_checknumber(L, 5));
      const float x3 = static_cast<float>(luaL_checknumber(L, 6));
      const float y3 = static_cast<float>(luaL_checknumber(L, 7));
      path->sk.cubicTo(SkFloatToScalar(x1),
                       SkFloatToScalar(y1),
                       SkFloatToScalar(x2),
                       SkFloatToScalar(y2),
                       SkFloatToScalar(x3),
                       SkFloatToScalar(y3));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "cubic_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x1 = static_cast<float>(luaL_checknumber(L, 2));
      const float y1 = static_cast<float>(luaL_checknumber(L, 3));
      const float x2 = static_cast<float>(luaL_checknumber(L, 4));
      const float y2 = static_cast<float>(luaL_checknumber(L, 5));
      const float weight = static_cast<float>(luaL_checknumber(L, 6));
      path->sk.conicTo(SkFloatToScalar(x1),
                       SkFloatToScalar(y1),
                       SkFloatToScalar(x2),
                       SkFloatToScalar(y2),
                       weight);
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "conic_to");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      path->sk.close();
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "close");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      path->sk.reset();
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "reset");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      path->sk.setFillType(CheckPathFillType(L, 2));
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "_set_fill_type");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      const float w = static_cast<float>(luaL_checknumber(L, 4));
      const float h = static_cast<float>(luaL_checknumber(L, 5));
      SkPathDirection direction = SkPathDirection::kCW;
      if (!lua_isnoneornil(L, 6)) {
        direction = CheckPathDirection(L, 6);
      }
      path->sk.addRect(SkRect::MakeXYWH(SkFloatToScalar(x),
                                        SkFloatToScalar(y),
                                        SkFloatToScalar(w),
                                        SkFloatToScalar(h)),
                       direction);
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "_add_rect");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      const float w = static_cast<float>(luaL_checknumber(L, 4));
      const float h = static_cast<float>(luaL_checknumber(L, 5));
      const float rx = static_cast<float>(luaL_checknumber(L, 6));
      const float ry = static_cast<float>(luaL_checknumber(L, 7));
      SkPathDirection direction = SkPathDirection::kCW;
      if (!lua_isnoneornil(L, 8)) {
        direction = CheckPathDirection(L, 8);
      }
      path->sk.addRRect(
          SkRRect::MakeRectXY(SkRect::MakeXYWH(SkFloatToScalar(x),
                                               SkFloatToScalar(y),
                                               SkFloatToScalar(w),
                                               SkFloatToScalar(h)),
                              SkFloatToScalar(rx),
                              SkFloatToScalar(ry)),
          direction);
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "_add_round_rect");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const float x = static_cast<float>(luaL_checknumber(L, 2));
      const float y = static_cast<float>(luaL_checknumber(L, 3));
      const float w = static_cast<float>(luaL_checknumber(L, 4));
      const float h = static_cast<float>(luaL_checknumber(L, 5));
      SkPathDirection direction = SkPathDirection::kCW;
      if (!lua_isnoneornil(L, 6)) {
        direction = CheckPathDirection(L, 6);
      }
      path->sk.addOval(SkRect::MakeXYWH(SkFloatToScalar(x),
                                        SkFloatToScalar(y),
                                        SkFloatToScalar(w),
                                        SkFloatToScalar(h)),
                       direction);
      lua_settop(L, 1);
      return 1;
    });
    lua_setfield(L, -2, "_add_oval");

    lua::PushFunction(L, [](lua_State *L) {
      LPath *path = lua::Check<LPath>(L, 1);
      const bool relative =
          !lua_isnoneornil(L, 2) && (lua_toboolean(L, 2) != 0);
      const SkString svg = SkParsePath::ToSVGString(
          path->snapshot(),
          relative ? SkParsePath::PathEncoding::Relative
                   : SkParsePath::PathEncoding::Absolute);
      lua_pushlstring(L, svg.c_str(), svg.size());
      return 1;
    });
    lua_setfield(L, -2, "to_svg_string");
  }
  lua_pop(L, 1);
}

void RegisterPathCanvasMethods(lua_State *L) {
  lua::PushFunction(L, [](lua_State *L) {
    lua::Check<LCanvas>(L, 1);

    SkPathFillType fill_type = SkPathFillType::kWinding;
    if (!lua_isnoneornil(L, 2)) {
      fill_type = CheckPathFillType(L, 2);
    }

    lua::New<LPath>(L, fill_type);
    return 1;
  });
  lua_setfield(L, -2, "_path");

  lua::PushFunction(L, [](lua_State *L) {
    lua::Check<LCanvas>(L, 1);
    const char *svg = luaL_checkstring(L, 2);
    if (!svg || !*svg) {
      return luaL_error(L, "path: SVG path string must not be empty");
    }

    std::optional<SkPath> parsed = SkParsePath::FromSVGString(svg);
    if (!parsed.has_value()) {
      return luaL_error(L, "path: invalid SVG path string");
    }

    if (!lua_isnoneornil(L, 3)) {
      parsed->setFillType(CheckPathFillType(L, 3));
    }

    lua::New<LPath>(L, *parsed);
    return 1;
  });
  lua_setfield(L, -2, "_path_svg");

  lua::PushFunction(L, [](lua_State *L) {
    LCanvas *canvas = lua::Check<LCanvas>(L, 1);
    LPath *path = lua::Check<LPath>(L, 2);

    LCanvas::LPaint *paint = nullptr;
    if (!lua_isnoneornil(L, 3)) {
      paint = lua::Check<LCanvas::LPaint>(L, 3);
    }

    SkPaint default_paint;
    canvas->sk()->drawPath(path->snapshot(), paint ? paint->sk : default_paint);
    return 0;
  });
  lua_setfield(L, -2, "_draw_path");

  lua::PushFunction(L, [](lua_State *L) {
    LCanvas *canvas = lua::Check<LCanvas>(L, 1);
    LPath *path = lua::Check<LPath>(L, 2);
    SkClipOp op = SkClipOp::kIntersect;
    if (!lua_isnoneornil(L, 3)) {
      op = CheckClipOp(L, 3);
    }

    bool anti_alias = false;
    if (!lua_isnoneornil(L, 4)) {
      luaL_checktype(L, 4, LUA_TBOOLEAN);
      anti_alias = lua_toboolean(L, 4) != 0;
    }

    canvas->sk()->clipPath(path->snapshot(), op, anti_alias);
    return 0;
  });
  lua_setfield(L, -2, "_clip_path");
}

void RegisterPathLuaHelpers(lua_State *L) {
  static constexpr char PATH_HELPERS[] = R"(
return function(canvas_mt, path_mt)
  local raw_path = assert(canvas_mt._path)
  local raw_path_svg = assert(canvas_mt._path_svg)
  local raw_draw_path = assert(canvas_mt._draw_path)
  local raw_clip_path = assert(canvas_mt._clip_path)
  local raw_path_set_fill_type = assert(path_mt._set_fill_type)
  local raw_path_add_rect = assert(path_mt._add_rect)
  local raw_path_add_round_rect = assert(path_mt._add_round_rect)
  local raw_path_add_oval = assert(path_mt._add_oval)

  local constants = assert(canvas_mt._constants)

  local path_fill_type_ = {
    winding = constants.path_fill_type.winding,
    even_odd = constants.path_fill_type.even_odd,
    inverse_winding = constants.path_fill_type.inverse_winding,
    inverse_even_odd = constants.path_fill_type.inverse_even_odd,
  }

  local clip_op_ = {
    difference = constants.clip_op.difference,
    intersect = constants.clip_op.intersect,
  }

  local allowed_path_keys = {
    svg = true,
    fill_type = true,
  }

  local path_direction_ = {
    cw = constants.path_direction.cw,
    clockwise = constants.path_direction.cw,
    ccw = constants.path_direction.ccw,
    counter_clockwise = constants.path_direction.ccw,
  }

  local function normalize_path_fill_type(fill_type, level)
    if fill_type == nil then
      return nil
    end
    if type(fill_type) == "number" then
      return fill_type
    end
    if type(fill_type) ~= "string" then
      error("path.fill_type must be a number or string", level or 3)
    end

    local mapped = path_fill_type_[fill_type]
    if mapped == nil then
      error(string.format("invalid path.fill_type '%s'", fill_type), level or 3)
    end
    return mapped
  end

  local function normalize_clip_op(op, level)
    if op == nil then
      return nil
    end
    if type(op) == "number" then
      return op
    end
    if type(op) ~= "string" then
      error("clip op must be a number or string", level or 3)
    end

    local mapped = clip_op_[op]
    if mapped == nil then
      error(string.format("invalid clip op '%s'", op), level or 3)
    end
    return mapped
  end

  local function normalize_path_direction(direction, level)
    if direction == nil then
      return nil
    end
    if type(direction) == "number" then
      return direction
    end
    if type(direction) ~= "string" then
      error("path direction must be a number or string", level or 3)
    end

    local mapped = path_direction_[direction]
    if mapped == nil then
      error(string.format("invalid path direction '%s'", direction), level or 3)
    end
    return mapped
  end

  local function compile_path(self, path, level)
    if path == nil then
      return raw_path(self, nil)
    end
    if type(path) == "userdata" then
      return path
    end
    if type(path) == "string" then
      return raw_path_svg(self, path, nil)
    end
    if type(path) ~= "table" then
      error("path must be a table, SVG path string, or compiled path", level or 3)
    end

    for key in pairs(path) do
      if not allowed_path_keys[key] then
        error(string.format("unknown path field '%s'", tostring(key)), level or 3)
      end
    end

    local fill_type = normalize_path_fill_type(path.fill_type, (level or 3) + 1)
    if path.svg ~= nil then
      return raw_path_svg(self, path.svg, fill_type)
    end
    return raw_path(self, fill_type)
  end

  function canvas_mt:path(path)
    return compile_path(self, path, 3)
  end

  function canvas_mt:draw_path(path, paint)
    return raw_draw_path(self, compile_path(self, path, 3), self:paint(paint))
  end

  function canvas_mt:clip_path(path, op, anti_alias)
    return raw_clip_path(
      self,
      compile_path(self, path, 3),
      normalize_clip_op(op, 3),
      anti_alias
    )
  end

  function path_mt:set_fill_type(fill_type)
    return raw_path_set_fill_type(self, normalize_path_fill_type(fill_type, 3))
  end

  function path_mt:add_rect(x, y, w, h, direction)
    return raw_path_add_rect(self, x, y, w, h, normalize_path_direction(direction, 3))
  end

  function path_mt:add_rrect(x, y, w, h, rx, ry, direction)
    return raw_path_add_round_rect(self, x, y, w, h, rx, ry, normalize_path_direction(direction, 3))
  end

  function path_mt:add_oval(x, y, w, h, direction)
    return raw_path_add_oval(self, x, y, w, h, normalize_path_direction(direction, 3))
  end
end
)";

  if (luaL_loadbuffer(L, PATH_HELPERS, std::strlen(PATH_HELPERS),
                      "canvas_path_helpers") != 0) {
    lua_error(L);
  }

  if (lua_pcall(L, 0, 1, 0) != 0) {
    lua_error(L);
  }

  lua_pushvalue(L, -2);
  luaL_getmetatable(L, LPath::MT);
  if (lua_pcall(L, 2, 0, 0) != 0) {
    lua_error(L);
  }
}

} // namespace luna::canvas
