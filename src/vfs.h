#ifndef LUNA_VFS_H
#define LUNA_VFS_H

#include "factory.h"
#include <functional>

namespace luna {

// Capture on the Lua thread; resolve/open on a worker.
struct AssetInput {
  std::shared_ptr<file::FileSource> source;
  std::string path;
  file::MappedFile blob;
  static AssetInput Check(lua_State *L, int index);
  file::FileStatus Map(file::MappedFile *out) const;
  file::FileStatus Open(std::unique_ptr<file::FileStream> *out) const;
};

void PushFileError(lua_State *L, const file::FileStatus &status);

class Vfs {
public:
  using StartJob = std::function<int(lua_State *, std::unique_ptr<AsyncJob>)>;
  explicit Vfs(std::string root = ".", std::string storage_root = "",
      std::string game_id = "");
  file::Vfs &files() { return *game_; }
  const file::Vfs &files() const { return *game_; }
  const std::string &UserRoot() const { return user_root_; }
  const std::string &Entry() const { return entry_; }
  void BindLua(lua_State *L, StartJob start);
  void BindSearcher(lua_State *L);

private:
  std::shared_ptr<file::Vfs> game_ = std::make_shared<file::Vfs>();
  std::shared_ptr<file::FileSource> user_, cache_;
  std::string user_root_, cache_root_;
  std::string entry_ = "main.lua";
};

} // namespace luna
#endif
