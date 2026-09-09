#include "lua_util.hpp"
#include "native_file_source.h"
#include "package_file_source.h"
#include "temp_directory.h"
#include "vfs.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
void Require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Run(lua_State *L, const char *script) {
  if (luaL_dostring(L, script) != LUA_OK)
    throw std::runtime_error(lua_tostring(L, -1));
}
void CheckFilesystem() {
  luna::test::TempDirectory temp;
  auto *L = luaL_newstate();
  luaL_openlibs(L);
  luna::Vfs vfs(temp.path().string(), (temp.path() / "storage").string());
  lua_newtable(L);
  // Inline executor isolates storage/serialization semantics. Real futures,
  // concurrency and await are exercised separately by engine.cmake.
  vfs.BindLua(L, [](lua_State *L, std::unique_ptr<luna::AsyncJob> job) {
    job->Invoke(L);
    job->Run();
    if (job->Rejected()) {
      lua_pushnil(L);
      luna::PushFileError(L, job->GetError());
      return 2;
    }
    return job->Finish(L);
  });
  lua_setglobal(L, "luna");
  Run(L, R"(
    assert(luna.save == nil)
    local fs, json = luna.fs.user, luna.json
    assert(fs:write('nested/a', 'hello'))
    local old = assert(fs:read('nested/a'))
    assert(old:size() == 5 and old:string() == 'hello')
    assert(fs:write('nested/a', 'replacement'))
    assert(old:string() == 'hello') -- replacement cannot mutate live mappings
    assert(fs:read('nested/a'):string() == 'replacement')
    assert(fs:write('copy', old))
    assert(fs:read('copy'):string() == 'hello')
    assert(fs:write('empty', ''))
    assert(fs:read('empty'):size() == 0)
    assert(fs:stat('nested').kind == 'directory')
    assert(fs:stat('copy').size == 5)
    assert(#fs:list('nested') == 1)
    local stream = assert(fs:open('copy'))
    assert(stream:read(2):string() == 'he')
    assert(stream:seek('end', -1) == 4)
    assert(stream:read(20):string() == 'o')
    assert(stream:read(20):size() == 0)
    assert(stream:close())
    assert(stream:close())
    local value, err = stream:read(1)
    assert(value == nil and err.code == 'io_error')
    local data = assert(json.encode({n=3, ok=true}))
    assert(fs:write('data.json', data))
    local decoded = assert(json.decode(assert(fs:read('data.json'))))
    assert(decoded.n == 3 and decoded.ok)
    value, err = json.decode('{bad')
    assert(value == nil and err.code == 'format_error')
    for _, path in ipairs({'../escape', '/absolute', 'bad' .. string.char(0) .. 'name'}) do
      value, err = fs:write(path, 'bad')
      assert(value == nil and err.code == 'invalid_path')
      value, err = fs:read(path)
      assert(value == nil and err.code == 'invalid_path')
      value, err = fs:remove(path)
      assert(value == nil and err.code == 'invalid_path')
    end
    value, err = luna.fs.game:write('no', 'bad')
    assert(value == nil and err.code == 'permission_denied')
    value, err = luna.fs.game:remove('copy')
    assert(value == nil and err.code == 'permission_denied')
    value, err = fs:read('missing')
    assert(value == nil and err.code == 'not_found')
    assert(fs:remove('copy'))
    value, err = fs:read('copy')
    assert(value == nil and err.code == 'not_found')
    value, err = fs:remove('nested') -- never recursive
    assert(value == nil and err)
    assert(luna.fs.cache:write('only-cache', 'disposable'))
    assert(fs:read('only-cache') == nil)
  )");

  auto outside = temp.path() / "outside";
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "data") << "original";
  std::error_code ec;
  std::filesystem::create_directory_symlink(
      outside, std::filesystem::path(vfs.UserRoot()) / "link", ec);
#if defined(_WIN32)
  Require(!ec || ec == std::errc::permission_denied || ec.value() == 1314,
      "symlink privilege");
#else
  Require(!ec, "create symlink");
#endif
  if (!ec) {
    std::filesystem::create_symlink(outside / "data",
        std::filesystem::path(vfs.UserRoot()) / "file-link", ec);
    Require(!ec, "create file symlink");
    Run(L, R"(
      local fs = luna.fs.user
      assert(fs:write('link/data', 'bad') == nil)
      assert(fs:read('link/data') == nil)
      assert(fs:remove('link/data') == nil)
      assert(fs:write('file-link', 'bad') == nil)
      assert(fs:read('file-link') == nil)
      assert(fs:remove('file-link') == nil)
    )");
    std::string value;
    std::ifstream(outside / "data") >> value;
    Require(value == "original", "outside target unchanged");
  }
  lua_close(L);
}
void CheckPackageStartup() {
  luna::test::TempDirectory temp;
  auto input = temp.path() / "input";
  auto ship = temp.path() / "ship";
  std::filesystem::create_directory(input);
  std::filesystem::create_directory(ship);
  std::ofstream(input / "game.json")
      << R"({"id":"archive-test","entry":"main.lua"})";
  std::ofstream(input / "main.lua") << "return {value=42}";
  std::shared_ptr<luna::file::FileSource> source;
  Require(bool(luna::file::OpenNativeFileSource(input.string(), {}, &source)),
      "input source");
  auto pack = [&](const char *name, std::vector<std::string> paths) {
    std::ofstream out(ship / name, std::ios::binary);
    Require(bool(luna::file::WritePackage(out, *source, std::move(paths))),
        "write package");
  };
  pack("game.luna", {"game.json", "main.lua"});
  pack("scripts.luna", {"main.lua"});
  pack("extra.luna", {"main.lua"});
  auto check = [&](const std::filesystem::path &root, bool multi) {
    luna::Vfs vfs(root.string(), (temp.path() / "storage").string());
    Require(vfs.Entry() == "main.lua", "package manifest entry");
    luna::file::MappedFile file;
    Require(bool(vfs.files().MapFile("main.lua", &file)),
        "package entry accessible");
    if (multi)
      Require(bool(vfs.files().MapFile("extra/main.lua", &file)),
          "second archive prefix");
    auto *L = luaL_newstate();
    luaL_openlibs(L);
    vfs.BindSearcher(L);
    Run(L, "assert(require('main').value == 42)");
    lua_close(L);
  };
  check(ship / "game.luna", false);
  std::ofstream(ship / "game.json") << R"({"id":"archive-test","mounts":[
    {"type":"package","path":"scripts.luna"},
    {"type":"package","path":"extra.luna","prefix":"extra"}]})";
  check(ship, true);
  std::filesystem::remove(ship / "extra.luna");
  bool rejected = false;
  try {
    luna::Vfs vfs(ship.string(), (temp.path() / "storage").string());
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Require(rejected, "missing declared archive fails startup");
}
void CheckNoStdFilesystem() {
  for (const char *path : {"src/vfs.h", "src/vfs.cpp", "src/file_vfs.cpp",
           "src/native_file_source.h", "src/native_file_source_linux.cpp",
           "src/native_file_source_windows.cpp",
           "src/package_file_source.cpp"}) {
    std::ifstream in(std::string(LUNA_SOURCE_DIR) + "/" + path);
    Require(bool(in), "read production source");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    Require(
        text.find("<filesystem>") == std::string::npos, "no filesystem header");
    Require(
        text.find("std::filesystem") == std::string::npos, "no std filesystem");
  }
}
} // namespace
int main() {
  CheckFilesystem();
  CheckPackageStartup();
  CheckNoStdFilesystem();
}
