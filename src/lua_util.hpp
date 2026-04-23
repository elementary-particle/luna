#ifndef LUNA_LUA_HELPER_H
#define LUNA_LUA_HELPER_H

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <functional>
#include <utility>

namespace lua {

inline int DestroyFunction(lua_State *L) {
  auto *f = static_cast<std::function<int(lua_State *)> *>(lua_touserdata(L, 1));
  f->~function();
  return 0;
}

inline void PushFunction(lua_State *L, std::function<int(lua_State *)> &&v) {
  void *ud = lua_newuserdata(L, sizeof(v));
  auto f = new (ud) std::function<int(lua_State *)>;
  *f = std::move(v);

  if (luaL_newmetatable(L, "luna.std_function")) {
    lua_pushcfunction(L, &DestroyFunction);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);

  lua_pushcclosure(
      L,
      [](lua_State *L) {
        auto *f = static_cast<std::function<int(lua_State *)> *>(
            lua_touserdata(L, lua_upvalueindex(1)));
        return (*f)(L);
      },
      1);
}

template <typename T> T *Test(lua_State *L, int i) {
  return static_cast<T *>(luaL_testudata(L, i, T::MT));
}

template <typename T> T *Check(lua_State *L, int i) {
  return static_cast<T *>(luaL_checkudata(L, i, T::MT));
}

template <typename T, typename... Args>
T *New(lua_State *L, Args &&...args) {
  void *raw = lua_newuserdata(L, sizeof(T));
  T *p = new (raw) T(std::forward<Args>(args)...);
  luaL_getmetatable(L, T::MT);
  lua_setmetatable(L, -2);
  return p;
}

/* The caller is responsible for popping the metatable from the stack */
template <typename T> int NewType(lua_State *L) {
  if (luaL_newmetatable(L, T::MT)) {
    lua_pushcfunction(L, [](lua_State *L) {
      T *p = Check<T>(L, 1);
      p->~T();
      return 0;
    });
    lua_setfield(L, -2, "__gc");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    return 1;
  }
  return 0;
}

}; // namespace lua

#endif
