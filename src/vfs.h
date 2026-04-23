#ifndef LUNA_VFS_H
#define LUNA_VFS_H

struct lua_State;

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <filesystem>
#include <string>

namespace luna {

struct VfsFile {
  SDL_IOStream *io = nullptr;
};

class VFS {
public:
  VFS();

  void SetLuaGlobals(lua_State *L);
  static VFS *GetInstance(lua_State *L);

  std::string SaveRoot() const { return save_root_.string(); }

  void BindLua(lua_State *L);
  std::filesystem::path ResolveSandboxedPath(const std::string &path) const;

  SDL_IOStream *OpenFile(std::string path);

private:
  std::filesystem::path save_root_;

  static int L_Open(lua_State *L);
  static int L_SaveWriteJson(lua_State *L);
  static int L_SaveReadJson(lua_State *L);

  static int L_FileReadAll(lua_State *L);
  static int L_FileWrite(lua_State *L);
  static int L_FileClose(lua_State *L);

  static VfsFile *CheckFile(lua_State *L, int idx);
};

} // namespace luna

#endif
