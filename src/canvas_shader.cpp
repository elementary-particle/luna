#include "canvas_bindings.h"

#include <cstring>
#include <vector>

#include <skia/core/SkColor.h>
#include <skia/core/SkPoint.h>
#include <skia/core/SkShader.h>
#include <skia/effects/SkGradient.h>

namespace luna::canvas {

namespace {

int LuaAbsIndex(lua_State *L, int idx) {
  if (idx > 0 || idx <= LUA_REGISTRYINDEX) {
    return idx;
  }
  return lua_gettop(L) + idx + 1;
}

SkTileMode CheckTileMode(lua_State *L, int idx) {
  return static_cast<SkTileMode>(luaL_checkinteger(L, idx));
}

bool ReadColorArray(lua_State *L, int idx, std::vector<SkColor4f> &colors) {
  idx = LuaAbsIndex(L, idx);
  const lua_Integer len = static_cast<lua_Integer>(lua_objlen(L, idx));
  if (len < 2) {
    return false;
  }

  colors.clear();
  colors.reserve(static_cast<size_t>(len));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, idx, i);
    colors.push_back(SkColor4f::FromColor(
        static_cast<SkColor>(luaL_checkinteger(L, -1))));
    lua_pop(L, 1);
  }
  return true;
}

bool ReadScalarArray(lua_State *L, int idx, std::vector<float> &positions,
                     lua_Integer expected) {
  idx = LuaAbsIndex(L, idx);
  const lua_Integer len = static_cast<lua_Integer>(lua_objlen(L, idx));
  if (len != expected) {
    return false;
  }

  positions.clear();
  positions.reserve(static_cast<size_t>(len));
  for (lua_Integer i = 1; i <= len; ++i) {
    lua_rawgeti(L, idx, i);
    positions.push_back(static_cast<float>(luaL_checknumber(L, -1)));
    lua_pop(L, 1);
  }
  return true;
}

} // namespace

void RegisterShaderBindings(lua_State *L) {
  lua::NewType<LShader>(L);
  lua_pop(L, 1);
}

void RegisterShaderCanvasMethods(lua_State *L) {
  lua::PushFunction(L, [](lua_State *L) {
    lua::Check<LCanvas>(L, 1);
    float x0 = static_cast<float>(luaL_checknumber(L, 2));
    float y0 = static_cast<float>(luaL_checknumber(L, 3));
    float x1 = static_cast<float>(luaL_checknumber(L, 4));
    float y1 = static_cast<float>(luaL_checknumber(L, 5));
    luaL_checktype(L, 6, LUA_TTABLE);

    std::vector<SkColor4f> colors;
    if (!ReadColorArray(L, 6, colors)) {
      return luaL_error(L, "shader.linear_gradient.colors must have at least 2 colors");
    }

    std::vector<float> positions;
    float *positions_ptr = nullptr;
    if (!lua_isnoneornil(L, 7)) {
      luaL_checktype(L, 7, LUA_TTABLE);
      if (!ReadScalarArray(L, 7, positions,
                           static_cast<lua_Integer>(colors.size()))) {
        return luaL_error(
            L,
            "shader.linear_gradient.positions must match colors length");
      }
      positions_ptr = positions.data();
    }

    SkTileMode tile_mode = SkTileMode::kClamp;
    if (!lua_isnoneornil(L, 8)) {
      tile_mode = CheckTileMode(L, 8);
    }

    const SkGradient grad(
        SkGradient::Colors(colors, SkSpan<const float>(positions_ptr, positions.size()),
                           tile_mode),
        {});
    const SkPoint points[2] = {
        SkPoint::Make(SkFloatToScalar(x0), SkFloatToScalar(y0)),
        SkPoint::Make(SkFloatToScalar(x1), SkFloatToScalar(y1)),
    };
    sk_sp<SkShader> shader = SkShaders::LinearGradient(
        points, grad);
    if (!shader) {
      return luaL_error(L, "shader.linear_gradient: failed to create shader");
    }

    lua::New<LShader>(L, std::move(shader));
    return 1;
  });
  lua_setfield(L, -2, "_shader_linear_gradient");

  lua::PushFunction(L, [](lua_State *L) {
    lua::Check<LCanvas>(L, 1);
    float cx = static_cast<float>(luaL_checknumber(L, 2));
    float cy = static_cast<float>(luaL_checknumber(L, 3));
    float radius = static_cast<float>(luaL_checknumber(L, 4));
    luaL_checktype(L, 5, LUA_TTABLE);

    std::vector<SkColor4f> colors;
    if (!ReadColorArray(L, 5, colors)) {
      return luaL_error(L, "shader.radial_gradient.colors must have at least 2 colors");
    }

    std::vector<float> positions;
    float *positions_ptr = nullptr;
    if (!lua_isnoneornil(L, 6)) {
      luaL_checktype(L, 6, LUA_TTABLE);
      if (!ReadScalarArray(L, 6, positions,
                           static_cast<lua_Integer>(colors.size()))) {
        return luaL_error(
            L,
            "shader.radial_gradient.positions must match colors length");
      }
      positions_ptr = positions.data();
    }

    SkTileMode tile_mode = SkTileMode::kClamp;
    if (!lua_isnoneornil(L, 7)) {
      tile_mode = CheckTileMode(L, 7);
    }

    const SkGradient grad(
        SkGradient::Colors(colors, SkSpan<const float>(positions_ptr, positions.size()),
                           tile_mode),
        {});
    sk_sp<SkShader> shader = SkShaders::RadialGradient(
        SkPoint::Make(SkFloatToScalar(cx), SkFloatToScalar(cy)),
        radius,
        grad);
    if (!shader) {
      return luaL_error(L, "shader.radial_gradient: failed to create shader");
    }

    lua::New<LShader>(L, std::move(shader));
    return 1;
  });
  lua_setfield(L, -2, "_shader_radial_gradient");
}

void RegisterShaderLuaHelpers(lua_State *L) {
  static constexpr char SHADER_HELPERS[] = R"(
return function(canvas_mt)
  local raw_shader_linear_gradient = assert(canvas_mt._shader_linear_gradient)
  local raw_shader_radial_gradient = assert(canvas_mt._shader_radial_gradient)

  local constants = assert(canvas_mt._constants)

  local tile_mode_ = {
    clamp = constants.tile_mode.clamp,
    ["repeat"] = constants.tile_mode["repeat"],
    mirror = constants.tile_mode.mirror,
    decal = constants.tile_mode.decal,
  }

  local allowed_shader_keys = {
    type = true,
    colors = true,
    positions = true,
    tile_mode = true,
    x0 = true,
    y0 = true,
    x1 = true,
    y1 = true,
    cx = true,
    cy = true,
    radius = true,
  }

  local function normalize_tile_mode(mode, level)
    if mode == nil then
      return nil
    end
    if type(mode) == "number" then
      return mode
    end
    if type(mode) ~= "string" then
      error("shader.tile_mode must be a number or string", level or 3)
    end

    local mapped = tile_mode_[mode]
    if mapped == nil then
      error(string.format("invalid shader.tile_mode '%s'", mode), level or 3)
    end
    return mapped
  end

  function canvas_mt:shader(shader)
    if shader == nil or type(shader) == "userdata" then
      return shader
    end
    if type(shader) ~= "table" then
      error("shader must be a table or compiled shader", 2)
    end

    for key in pairs(shader) do
      if not allowed_shader_keys[key] then
        error(string.format("unknown shader field '%s'", tostring(key)), 2)
      end
    end

    if shader.type == "linear_gradient" then
      return raw_shader_linear_gradient(
        self,
        shader.x0,
        shader.y0,
        shader.x1,
        shader.y1,
        shader.colors,
        shader.positions,
        normalize_tile_mode(shader.tile_mode, 3)
      )
    end

    if shader.type == "radial_gradient" then
      return raw_shader_radial_gradient(
        self,
        shader.cx,
        shader.cy,
        shader.radius,
        shader.colors,
        shader.positions,
        normalize_tile_mode(shader.tile_mode, 3)
      )
    end

    error(string.format("invalid shader.type '%s'", tostring(shader.type)), 2)
  end
end
)";

  if (luaL_loadbuffer(L, SHADER_HELPERS, std::strlen(SHADER_HELPERS),
                      "canvas_shader_helpers") != 0) {
    lua_error(L);
  }

  if (lua_pcall(L, 0, 1, 0) != 0) {
    lua_error(L);
  }

  lua_pushvalue(L, -2);
  if (lua_pcall(L, 1, 0, 0) != 0) {
    lua_error(L);
  }
}

} // namespace luna::canvas
