#include "vfs.h"

#include <SDL3/SDL_filesystem.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "lua_json.h"
#include "lua_util.hpp"

namespace luna {

namespace {

std::filesystem::path NormalizeRoot(std::filesystem::path root) {
  if (root.empty()) {
    root = std::filesystem::current_path();
  }

  std::error_code ec;
  const std::filesystem::path absolute_root =
      std::filesystem::absolute(root, ec);
  if (!ec) {
    return absolute_root.lexically_normal();
  }

  return root.lexically_normal();
}

} // namespace

Vfs::Vfs(std::filesystem::path root) : root_(NormalizeRoot(std::move(root))) {
  const char *pref_path = SDL_GetPrefPath("luna", "luna");
  if (pref_path && *pref_path) {
    save_root_ = std::filesystem::path(pref_path) / "save";
  } else {
    save_root_ = std::filesystem::path("save");
  }

  std::error_code ec;
  std::filesystem::create_directories(save_root_, ec);

  auto native_root = asset::OpenNativeAssetSource(root_);
  if (native_root) {
    (void)assets_.Mount("/", std::move(native_root).value());
  }
}

std::filesystem::path Vfs::ResolvePathUnderRoot(
    const std::filesystem::path &root, const std::filesystem::path &path) {
  if (path.empty()) {
    throw std::runtime_error("path is empty");
  }

  if (path.is_absolute()) {
    throw std::runtime_error("absolute paths are not allowed");
  }

  std::filesystem::path clean;
  for (const auto &part : path) {
    const std::string seg = part.string();
    if (seg == "." || seg.empty()) {
      continue;
    }
    if (seg == "..") {
      throw std::runtime_error("path traversal is not allowed");
    }
    clean /= part;
  }

  if (clean.empty()) {
    throw std::runtime_error("resolved path is empty");
  }

  return (root / clean).lexically_normal();
}

std::filesystem::path Vfs::ResolveSandboxedPath(const std::string &path) const {
  return ResolvePathUnderRoot(save_root_, std::filesystem::path(path));
}

int Vfs::L_SaveWriteJson(lua_State *L) {
  auto *vfs = static_cast<Vfs *>(lua_touserdata(L, lua_upvalueindex(1)));
  const char *path = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  std::filesystem::path resolved;
  try {
    resolved = vfs->ResolveSandboxedPath(path);
  } catch (const std::exception &ex) {
    return luaL_error(L, "save.write_json: %s", ex.what());
  }

  std::string json;
  try {
    json = lua_json::Encode(L, 2);
  } catch (const std::exception &ex) {
    return luaL_error(L, "save.write_json: %s", ex.what());
  }

  std::error_code ec;
  std::filesystem::create_directories(resolved.parent_path(), ec);

  std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
  if (!out) {
    return luaL_error(L, "save.write_json: open failed");
  }
  out << json;
  if (!out.good()) {
    return luaL_error(L, "save.write_json: write failed");
  }

  lua_pushboolean(L, 1);
  return 1;
}

int Vfs::L_SaveReadJson(lua_State *L) {
  auto *vfs = static_cast<Vfs *>(lua_touserdata(L, lua_upvalueindex(1)));
  const char *path = luaL_checkstring(L, 1);

  std::filesystem::path resolved;
  try {
    resolved = vfs->ResolveSandboxedPath(path);
  } catch (const std::exception &ex) {
    lua_pushnil(L);
    lua_pushstring(L, ex.what());
    return 2;
  }

  std::ifstream in(resolved, std::ios::binary);
  if (!in) {
    lua_pushnil(L);
    lua_pushstring(L, "file not found");
    return 2;
  }

  std::stringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();

  try {
    lua_json::Decode(L, json);
    return 1;
  } catch (const std::exception &ex) {
    lua_pushnil(L);
    lua_pushstring(L, ex.what());
    return 2;
  }
}

void Vfs::BindLua(lua_State *L) {
  lua_newtable(L);

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &Vfs::L_SaveWriteJson, 1);
  lua_setfield(L, -2, "write_json");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &Vfs::L_SaveReadJson, 1);
  lua_setfield(L, -2, "read_json");

  lua_pushstring(L, SaveRoot().c_str());
  lua_setfield(L, -2, "root");

  lua_setfield(L, -2, "save");
}

} // namespace luna
