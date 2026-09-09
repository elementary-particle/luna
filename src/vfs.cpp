#include "vfs.h"

#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "lua_json.h"
#include "lua_util.hpp"
#include "native_file_source.h"
#include "package_file_source.h"

namespace luna {
namespace {
struct Blob {
  static constexpr const char *MT = "luna.Blob";
  file::MappedFile data;
};
struct FileRef {
  static constexpr const char *MT = "luna.FileRef";
  AssetInput input;
};
struct Root {
  static constexpr const char *MT = "luna.Filesystem";
  std::shared_ptr<file::FileSource> source;
  std::string writable_root;
  Vfs::StartJob start;
};
struct StreamState {
  std::unique_ptr<file::FileStream> stream;
  bool busy = false; // Lua-thread only; one outstanding operation per stream.
};
struct Stream {
  static constexpr const char *MT = "luna.ReadStream";
  std::shared_ptr<StreamState> state;
  Vfs::StartJob start;
};

std::string CheckString(lua_State *L, int index) {
  size_t size;
  const char *data = luaL_checklstring(L, index, &size);
  return {data, size};
}

void PushInfo(lua_State *L, const file::FileInfo &info) {
  lua_newtable(L);
  lua_pushlstring(L, info.path.data(), info.path.size());
  lua_setfield(L, -2, "path");
  lua_pushstring(
      L, info.kind == file::FileKind::kDirectory ? "directory" : "file");
  lua_setfield(L, -2, "kind");
  lua_pushnumber(L, static_cast<lua_Number>(info.size));
  lua_setfield(L, -2, "size");
}

enum class Operation { Read, Open, Stat, List, Write, Remove };
class FileJob final : public AsyncJob {
public:
  FileJob(Root root, Operation op) : root_(std::move(root)), op_(op) {}
  void Invoke(lua_State *L) override {
    path_ = CheckString(L, 2);
    if (op_ == Operation::Write) {
      if (auto *blob = lua::Test<Blob>(L, 3))
        bytes_ = blob->data;
      else {
        auto text = CheckString(L, 3);
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        bytes_ = file::MappedFile::FromOwnedBuffer(std::move(bytes), {});
      }
    }
  }
  void Run() override {
    std::string normalized;
    status_ = file::NormalizePath(
        path_, op_ == Operation::List || op_ == Operation::Stat, &normalized);
    if (!status_)
      return;
    if ((op_ == Operation::Write || op_ == Operation::Remove) &&
        root_.writable_root.empty()) {
      status_ = file::FileStatus::Error(file::FileStatusCode::kPermissionDenied,
          op_ == Operation::Write ? "write" : "remove", path_,
          "filesystem is read-only");
      return;
    }
    // Mutable roots bypass the VFS cache: replacement must be immediately
    // visible.
    file::ReadOptions options;
    if (!root_.writable_root.empty())
      options.cache_policy = file::CachePolicy::kBypassCache;
    switch (op_) {
    case Operation::Read:
      status_ = root_.source->MapFile(path_, options, &bytes_);
      break;
    case Operation::Open:
      status_ = root_.source->OpenFile(path_, {}, &stream_);
      break;
    case Operation::Stat:
      status_ = root_.source->Stat(path_, &info_);
      break;
    case Operation::List:
      status_ = root_.source->List(path_, &entries_);
      break;
    case Operation::Write:
      status_ = file::NativeAtomicWriteUnderRoot(
          root_.writable_root, path_, bytes_.bytes());
      break;
    case Operation::Remove:
      status_ = file::NativeRemoveFileUnderRoot(root_.writable_root, path_);
      break;
    }
  }
  int Finish(lua_State *L) override {
    switch (op_) {
    case Operation::Read:
      lua::New<Blob>(L, std::move(bytes_));
      break;
    case Operation::Open: {
      auto state = std::make_shared<StreamState>();
      state->stream = std::move(stream_);
      lua::New<Stream>(L, std::move(state), root_.start);
      break;
    }
    case Operation::Stat:
      PushInfo(L, info_);
      break;
    case Operation::List:
      lua_newtable(L);
      for (size_t i = 0; i < entries_.size(); ++i) {
        PushInfo(L, entries_[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
      }
      break;
    default:
      lua_pushboolean(L, true);
      break;
    }
    return 1;
  }

private:
  Root root_;
  Operation op_;
  std::string path_;
  file::MappedFile bytes_;
  std::unique_ptr<file::FileStream> stream_;
  file::FileInfo info_;
  std::vector<file::FileInfo> entries_;
};

enum class StreamOp { Read, Seek, Close };
class StreamJob final : public AsyncJob {
public:
  StreamJob(std::shared_ptr<StreamState> state, StreamOp op)
      : state_(std::move(state)), op_(op) {}
  ~StreamJob() override {
    if (claimed_)
      state_->busy = false;
  }
  void Invoke(lua_State *L) override {
    if (state_->busy)
      luaL_error(L, "stream already has an outstanding operation");
    if (op_ == StreamOp::Read) {
      const auto size = luaL_checknumber(L, 2);
      // Bound individual allocations. Large reads should be performed in
      // chunks.
      if (!(size >= 0 && size <= 64 * 1024 * 1024) || size != std::floor(size))
        luaL_argerror(L, 2, "expected byte count in [0, 64 MiB]");
      count_ = static_cast<size_t>(size);
    } else if (op_ == StreamOp::Seek) {
      const char *origins[] = {"set", "cur", "end", nullptr};
      int origin = luaL_checkoption(L, 2, "set", origins);
      origin_ = origin == 0 ? std::ios::beg
          : origin == 1     ? std::ios::cur
                            : std::ios::end;
      const auto offset = luaL_optnumber(L, 3, 0);
      if (!std::isfinite(offset) || offset != std::floor(offset) ||
          offset < -9007199254740991.0 || offset > 9007199254740991.0)
        luaL_argerror(L, 3, "expected exact integer offset");
      offset_ = static_cast<int64_t>(offset);
    }
    claimed_ = state_->busy = true;
  }
  void Run() override {
    if (op_ == StreamOp::Close) {
      state_->stream.reset();
      return;
    }
    if (!state_->stream) {
      status_ = file::FileStatus::Error(
          file::FileStatusCode::kIoError, "stream", "", "stream is closed");
      return;
    }
    if (op_ == StreamOp::Seek) {
      status_ = state_->stream->Seek(offset_, origin_);
      position_ = state_->stream->position();
    } else {
      bytes_.resize(count_);
      size_t read = 0;
      status_ = state_->stream->Read(bytes_, &read);
      bytes_.resize(read);
    }
  }
  int Finish(lua_State *L) override {
    if (op_ == StreamOp::Read)
      lua::New<Blob>(
          L, file::MappedFile::FromOwnedBuffer(std::move(bytes_), {}));
    else if (op_ == StreamOp::Seek)
      lua_pushnumber(L, static_cast<lua_Number>(position_));
    else
      lua_pushboolean(L, true);
    return 1;
  }

private:
  std::shared_ptr<StreamState> state_;
  StreamOp op_;
  bool claimed_ = false;
  size_t count_ = 0;
  int64_t offset_ = 0;
  uint64_t position_ = 0;
  std::ios::seekdir origin_ = std::ios::beg;
  std::vector<std::byte> bytes_;
};

class BlobStream final : public file::FileStream {
public:
  explicit BlobStream(file::MappedFile data) : data_(std::move(data)) {}
  const file::FileInfo &info() const noexcept override { return data_.info(); }
  uint64_t position() const noexcept override { return position_; }
  bool eof() const noexcept override { return position_ >= data_.size(); }
  file::FileStatus Read(std::span<std::byte> out, size_t *read) override {
    *read = static_cast<size_t>(
        std::min<uint64_t>(out.size(), data_.size() - position_));
    if (*read)
      std::memcpy(out.data(), data_.data() + position_, *read);
    position_ += *read;
    return {};
  }
  file::FileStatus Seek(int64_t offset, std::ios::seekdir origin) override {
    auto base = origin == std::ios::beg ? 0
        : origin == std::ios::cur       ? position_
                                        : data_.size();
    if ((offset < 0 && static_cast<uint64_t>(-(offset + 1)) + 1 > base) ||
        (offset >= 0 && static_cast<uint64_t>(offset) > data_.size() - base))
      return file::FileStatus::Error(
          file::FileStatusCode::kIoError, "seek", "", "seek outside blob");
    position_ = base + offset;
    return {};
  }

private:
  file::MappedFile data_;
  uint64_t position_ = 0;
};

void RequireStatus(const file::FileStatus &status) {
  if (!status)
    throw std::runtime_error(status.operation() + ": " + status.message());
}

// A view keeps resolution anchored to the original native source, including
// its descriptor-based symlink checks. Manifest paths never become new roots.
class DirectoryView final : public file::FileSource {
public:
  DirectoryView(std::shared_ptr<file::FileSource> source, std::string prefix)
      : source_(std::move(source)), prefix_(std::move(prefix)) {}
  std::string_view DebugName() const noexcept override {
    return "directory_view";
  }
  file::FileStatus Stat(std::string_view path, file::FileInfo *out) override {
    return source_->Stat(Path(path), out);
  }
  file::FileStatus List(
      std::string_view path, std::vector<file::FileInfo> *out) override {
    auto status = source_->List(Path(path), out);
    if (status)
      for (auto &entry : *out)
        entry.path.erase(0, prefix_.size() + 1);
    return status;
  }
  file::FileStatus OpenFile(std::string_view path,
      const file::ReadOptions &options,
      std::unique_ptr<file::FileStream> *out) override {
    return source_->OpenFile(Path(path), options, out);
  }
  file::FileStatus MapFile(std::string_view path,
      const file::ReadOptions &options, file::MappedFile *out) override {
    return source_->MapFile(Path(path), options, out);
  }

private:
  std::string Path(std::string_view path) const {
    return prefix_ + "/" + std::string(path);
  }
  std::shared_ptr<file::FileSource> source_;
  std::string prefix_;
};

std::string ManifestString(
    lua_State *L, const char *key, std::string fallback = "") {
  lua_getfield(L, -1, key);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return fallback;
  }
  if (lua_type(L, -1) != LUA_TSTRING)
    throw std::runtime_error(
        std::string("game.json: expected string for ") + key);
  auto result = CheckString(L, -1);
  lua_pop(L, 1);
  return result;
}
} // namespace

void PushFileError(lua_State *L, const file::FileStatus &status) {
  static constexpr const char *codes[] = {"ok", "not_found", "invalid_path",
      "not_a_file", "is_directory", "not_a_directory", "permission_denied",
      "io_error", "format_error", "unsupported", "budget_exceeded"};
  lua_newtable(L);
  lua_pushstring(L, codes[static_cast<size_t>(status.code())]);
  lua_setfield(L, -2, "code");
  lua_pushstring(L, status.operation().c_str());
  lua_setfield(L, -2, "operation");
  lua_pushlstring(L, status.path().data(), status.path().size());
  lua_setfield(L, -2, "path");
  lua_pushstring(L, status.message().c_str());
  lua_setfield(L, -2, "message");
  lua_pushnumber(L, static_cast<lua_Number>(status.native_code()));
  lua_setfield(L, -2, "native_code");
}

AssetInput AssetInput::Check(lua_State *L, int index) {
  if (auto *ref = lua::Test<FileRef>(L, index))
    return ref->input;
  auto *blob = lua::Check<Blob>(L, index);
  return {nullptr, "", blob->data};
}
file::FileStatus AssetInput::Map(file::MappedFile *out) const {
  if (source)
    return source->MapFile(path, {}, out);
  *out = blob;
  return {};
}
file::FileStatus AssetInput::Open(
    std::unique_ptr<file::FileStream> *out) const {
  if (source)
    return source->OpenFile(path, {}, out);
  *out = std::make_unique<BlobStream>(blob);
  return {};
}

Vfs::Vfs(std::string root, std::string storage_root, std::string game_id) {
  std::shared_ptr<file::FileSource> source;
  auto status = file::OpenNativeFileSource(root, {}, &source);
  if (status.code() == file::FileStatusCode::kNotADirectory)
    status = file::OpenPackageFileSource(root, {}, &source);
  RequireStatus(status);
  RequireStatus(game_->Mount("/", source));
  game_->SetMemoryBudget(64 * 1024 * 1024);

  file::MappedFile manifest;
  auto manifest_status = source->MapFile("game.json", {}, &manifest);
  if (manifest_status) {
    std::unique_ptr<lua_State, decltype(&lua_close)> state(
        luaL_newstate(), lua_close);
    if (!state)
      throw std::bad_alloc();
    auto *L = state.get();
    lua_json::Decode(L,
        std::string_view(manifest.empty()
                ? ""
                : reinterpret_cast<const char *>(manifest.data()),
            manifest.size()));
    if (!lua_istable(L, -1))
      throw std::runtime_error("game.json: expected object");
    auto manifest_id = ManifestString(L, "id");
    if (manifest_id.empty())
      throw std::runtime_error("game.json: id is required");
    if (game_id.empty())
      game_id = manifest_id;
    entry_ = ManifestString(L, "entry", "main.lua");
    std::string normalized;
    RequireStatus(file::NormalizePath(entry_, false, &normalized));
    entry_ = std::move(normalized);
    lua_getfield(L, -1, "mounts");
    if (!lua_isnil(L, -1)) {
      if (!lua_istable(L, -1))
        throw std::runtime_error("game.json: mounts must be an array");
      for (size_t i = 1; i <= lua_objlen(L, -1); ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i));
        if (!lua_istable(L, -1))
          throw std::runtime_error("game.json: invalid mount");
        auto type = ManifestString(L, "type", "directory");
        if (type != "directory" && type != "package")
          throw std::runtime_error("game.json: unknown mount type");
        auto path = ManifestString(L, "path");
        RequireStatus(file::NormalizePath(path, false, &normalized));
        file::FileInfo info;
        RequireStatus(source->Stat(normalized, &info));
        if (type == "directory" && info.kind != file::FileKind::kDirectory)
          throw std::runtime_error(
              "game.json: directory mount is not a directory");
        auto prefix = ManifestString(L, "prefix", "/");
        lua_getfield(L, -1, "priority");
        auto priority = lua_isnil(L, -1) ? 0 : lua_tonumber(L, -1);
        if ((!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TNUMBER) ||
            !std::isfinite(priority) || priority != std::floor(priority) ||
            priority < std::numeric_limits<int>::min() ||
            priority > std::numeric_limits<int>::max())
          throw std::runtime_error("game.json: invalid mount priority");
        lua_pop(L, 1);
        std::shared_ptr<file::FileSource> mounted;
        if (type == "package") {
          RequireStatus(file::OpenPackageFileSource(
              *source, normalized, {}, &mounted));
        } else {
          mounted = std::make_shared<DirectoryView>(source, normalized);
        }
        RequireStatus(game_->Mount(prefix, mounted, {static_cast<int>(priority)}));
        lua_pop(L, 1);
      }
    }
  } else if (manifest_status.code() != file::FileStatusCode::kNotFound) {
    RequireStatus(manifest_status);
  }
  if (game_id.empty())
    game_id = "luna-development";
  if (game_id.empty() ||
      game_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRST"
                                "UVWXYZ0123456789-_.") != std::string::npos ||
      game_id == "." || game_id == "..")
    throw std::invalid_argument(
        "game id must contain only letters, digits, '.', '-' or '_'");
  if (storage_root.empty()) {
    char *pref = SDL_GetPrefPath("luna", game_id.c_str());
    if (!pref)
      throw std::runtime_error("cannot locate application storage");
    storage_root = pref;
    SDL_free(pref);
  }
  user_root_ = storage_root + "/user";
  cache_root_ = storage_root + "/cache";
  RequireStatus(file::NativeCreateDirectories(user_root_));
  RequireStatus(file::NativeCreateDirectories(cache_root_));
  RequireStatus(file::OpenNativeFileSource(user_root_, {}, &user_));
  RequireStatus(file::OpenNativeFileSource(cache_root_, {}, &cache_));
}

void Vfs::BindLua(lua_State *L, StartJob start) {
  lua::NewType<Blob>(L);
  lua_pushcfunction(L, [](lua_State *L) {
    auto &data = lua::Check<Blob>(L, 1)->data;
    lua_pushlstring(L,
        data.empty() ? "" : reinterpret_cast<const char *>(data.data()),
        data.size());
    return 1;
  });
  lua_setfield(L, -2, "string");
  lua_pushcfunction(L, [](lua_State *L) {
    lua_pushnumber(L, lua::Check<Blob>(L, 1)->data.size());
    return 1;
  });
  lua_setfield(L, -2, "size");
  lua_pop(L, 1);
  lua::NewType<FileRef>(L);
  lua_pop(L, 1);
  lua::NewType<Root>(L);
  for (auto [name, op] :
      {std::pair{"read", Operation::Read}, {"open", Operation::Open},
          {"stat", Operation::Stat}, {"list", Operation::List},
          {"write", Operation::Write}, {"remove", Operation::Remove}}) {
    lua::PushFunction(L, [op](lua_State *L) {
      auto *root = lua::Check<Root>(L, 1);
      return root->start(L, std::make_unique<FileJob>(*root, op));
    });
    lua_setfield(L, -2, name);
  }
  lua_pushcfunction(L, [](lua_State *L) {
    auto *root = lua::Check<Root>(L, 1);
    auto path = CheckString(L, 2);
    std::string normalized;
    auto status = file::NormalizePath(path, false, &normalized);
    if (!status)
      return luaL_argerror(L, 2, status.message().c_str());
    lua::New<FileRef>(L, AssetInput{root->source, std::move(normalized), {}});
    return 1;
  });
  lua_setfield(L, -2, "ref");
  lua_pop(L, 1);
  lua::NewType<Stream>(L);
  for (auto [name, op] : {std::pair{"read", StreamOp::Read},
           {"seek", StreamOp::Seek}, {"close", StreamOp::Close}}) {
    lua::PushFunction(L, [op](lua_State *L) {
      auto *stream = lua::Check<Stream>(L, 1);
      return stream->start(L, std::make_unique<StreamJob>(stream->state, op));
    });
    lua_setfield(L, -2, name);
  }
  lua_pop(L, 1);

  lua_newtable(L);
  lua::New<Root>(L, game_, std::string{}, start);
  lua_setfield(L, -2, "game");
  lua::New<Root>(L, user_, user_root_, start);
  lua_setfield(L, -2, "user");
  lua::New<Root>(L, cache_, cache_root_, start);
  lua_setfield(L, -2, "cache");
  lua_setfield(L, -2, "fs");

  lua_newtable(L);
  lua_pushcfunction(L, [](lua_State *L) {
    try {
      auto json = lua_json::Encode(L, 1);
      lua_pushlstring(L, json.data(), json.size());
      return 1;
    } catch (const std::exception &ex) {
      lua_pushnil(L);
      PushFileError(L,
          file::FileStatus::Error(file::FileStatusCode::kFormatError,
              "json.encode", "", ex.what()));
      return 2;
    }
  });
  lua_setfield(L, -2, "encode");
  lua_pushcfunction(L, [](lua_State *L) {
    std::string text;
    if (auto *blob = lua::Test<Blob>(L, 1)) {
      if (!blob->data.empty())
        text.assign(reinterpret_cast<const char *>(blob->data.data()),
            blob->data.size());
    } else
      text = CheckString(L, 1);
    try {
      lua_json::Decode(L, text);
      return 1;
    } catch (const std::exception &ex) {
      lua_pushnil(L);
      PushFileError(L,
          file::FileStatus::Error(file::FileStatusCode::kFormatError,
              "json.decode", "", ex.what()));
      return 2;
    }
  });
  lua_setfield(L, -2, "decode");
  lua_setfield(L, -2, "json");
}

void Vfs::BindSearcher(lua_State *L) {
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "loaders");
  // Preserve preload (including luna), replace host Lua/C searchers.
  lua::PushFunction(L, [source = game_](lua_State *L) {
    auto module = CheckString(L, 1);
    std::replace(module.begin(), module.end(), '.', '/');
    for (const auto &suffix : {std::string(".lua"), std::string("/init.lua")}) {
      file::MappedFile bytes;
      auto status = source->MapFile(module + suffix, &bytes);
      if (!status) {
        if (status.code() == file::FileStatusCode::kNotFound)
          continue;
        return luaL_error(L, "require: %s", status.message().c_str());
      }
      auto name = "@" + module + suffix;
      if (luaL_loadbuffer(L,
              bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data()),
              bytes.size(), name.c_str()) != 0)
        return lua_error(L);
      return 1;
    }
    lua_pushfstring(L, "\n\tno game module '%s'", module.c_str());
    return 1;
  });
  lua_rawseti(L, -2, 2);
  const int count = static_cast<int>(lua_objlen(L, -1));
  for (int i = 3; i <= count; ++i) {
    lua_pushnil(L);
    lua_rawseti(L, -2, i);
  }
  lua_pop(L, 2);
}
} // namespace luna
