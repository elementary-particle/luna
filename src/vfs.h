#ifndef LUNA_VFS_H
#define LUNA_VFS_H

struct lua_State;

#include <filesystem>
#include <string>

#include "asset_vfs.h"

namespace luna {

class Vfs {
public:
  explicit Vfs(std::filesystem::path root = std::filesystem::current_path());

  std::string SaveRoot() const { return save_root_.string(); }
  const std::filesystem::path &Root() const { return root_; }

  asset::Vfs &assets() { return assets_; }
  const asset::Vfs &assets() const { return assets_; }

  void BindLua(lua_State *L);
  std::filesystem::path ResolveSandboxedPath(const std::string &path) const;

private:
  static std::filesystem::path ResolvePathUnderRoot(
      const std::filesystem::path &root, const std::filesystem::path &path);

  std::filesystem::path root_;
  std::filesystem::path save_root_;
  asset::Vfs assets_;

  static int L_SaveWriteJson(lua_State *L);
  static int L_SaveReadJson(lua_State *L);
};

} // namespace luna

#endif
