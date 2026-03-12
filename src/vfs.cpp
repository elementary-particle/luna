#include "vfs.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace luna {

static constexpr const char *VFS_PTR_KEY = "luna.vfs_ptr";
static constexpr const char *LUA_VFS_FILE_MT = "luna.VfsFile";

static int LuaAbsIndex(lua_State *L, int idx) {
  if (idx > 0 || idx <= LUA_REGISTRYINDEX)
    return idx;
  return lua_gettop(L) + idx + 1;
}

static bool LuaIsInteger(lua_State *L, int idx) {
  const lua_Number n = lua_tonumber(L, idx);
  const lua_Integer i = lua_tointeger(L, idx);
  return n == static_cast<lua_Number>(i);
}

VFS::VFS() {
  const char *pref_path = SDL_GetPrefPath("luna", "luna");
  if (pref_path && *pref_path) {
    save_root_ = std::filesystem::path(pref_path) / "save";
  } else {
    save_root_ = std::filesystem::path("save");
  }
  std::error_code ec;
  std::filesystem::create_directories(save_root_, ec);
}

void VFS::SetLuaGlobals(lua_State *L) {
  lua_pushlightuserdata(L, this);
  lua_setfield(L, LUA_REGISTRYINDEX, VFS_PTR_KEY);
}

VFS *VFS::GetInstance(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, VFS_PTR_KEY);
  auto *vfs = static_cast<VFS *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return vfs;
}

std::filesystem::path VFS::ResolveSandboxedPath(const std::string &path) const {
  if (path.empty()) {
    throw std::runtime_error("path is empty");
  }

  std::filesystem::path rel(path);
  if (rel.is_absolute()) {
    throw std::runtime_error("absolute paths are not allowed");
  }

  std::filesystem::path clean;
  for (const auto &part : rel) {
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

  return save_root_ / clean;
}

SDL_IOStream *VFS::OpenFile(std::string path) {
  return SDL_IOFromFile(path.c_str(), "rb");
}

static std::string JsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        char buf[7];
        SDL_snprintf(buf, sizeof(buf), "\\u%04x",
                     (unsigned int)(unsigned char)ch);
        out += buf;
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return out;
}

static std::string LuaValueToJson(lua_State *L, int idx);

static std::string LuaTableToJson(lua_State *L, int idx) {
  idx = LuaAbsIndex(L, idx);

  bool is_array = true;
  size_t max_index = 0;
  size_t count = 0;

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    ++count;
    if (!LuaIsInteger(L, -2)) {
      is_array = false;
    } else {
      lua_Integer k = lua_tointeger(L, -2);
      if (k <= 0) {
        is_array = false;
      } else {
        max_index = std::max(max_index, static_cast<size_t>(k));
      }
    }
    lua_pop(L, 1);
  }

  if (is_array && max_index == count) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 1; i <= max_index; ++i) {
      if (i > 1)
        oss << ",";
      lua_rawgeti(L, idx, static_cast<int>(i));
      oss << LuaValueToJson(L, -1);
      lua_pop(L, 1);
    }
    oss << "]";
    return oss.str();
  }

  std::vector<std::string> keys;
  keys.reserve(count);

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    if (!lua_isstring(L, -2)) {
      lua_pop(L, 2);
      throw std::runtime_error("json object keys must be strings");
    }
    keys.emplace_back(lua_tostring(L, -2));
    lua_pop(L, 1);
  }

  std::sort(keys.begin(), keys.end());

  std::ostringstream oss;
  oss << "{";
  bool first = true;
  for (const auto &k : keys) {
    lua_getfield(L, idx, k.c_str());
    if (!first)
      oss << ",";
    first = false;
    oss << '"' << JsonEscape(k) << "\":" << LuaValueToJson(L, -1);
    lua_pop(L, 1);
  }
  oss << "}";
  return oss.str();
}

static std::string LuaValueToJson(lua_State *L, int idx) {
  const int t = lua_type(L, idx);
  switch (t) {
  case LUA_TNIL:
    return "null";
  case LUA_TBOOLEAN:
    return lua_toboolean(L, idx) ? "true" : "false";
  case LUA_TNUMBER: {
    if (LuaIsInteger(L, idx)) {
      return std::to_string((long long)lua_tointeger(L, idx));
    }
    const lua_Number n = lua_tonumber(L, idx);
    if (n != n) {
      throw std::runtime_error("json does not support NaN");
    }
    if (n == std::numeric_limits<lua_Number>::infinity() ||
        n == -std::numeric_limits<lua_Number>::infinity()) {
      throw std::runtime_error("json does not support infinity");
    }
    char buf[64];
    SDL_snprintf(buf, sizeof(buf), "%.17g", (double)n);
    return buf;
  }
  case LUA_TSTRING:
    return '"' + JsonEscape(lua_tostring(L, idx)) + '"';
  case LUA_TTABLE:
    return LuaTableToJson(L, idx);
  default:
    throw std::runtime_error("unsupported lua type for json serialization");
  }
}

struct JsonParser {
  const std::string &s;
  size_t i = 0;

  explicit JsonParser(const std::string &src) : s(src) {}

  void SkipWs() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    }
  }

  void Expect(char ch) {
    SkipWs();
    if (i >= s.size() || s[i] != ch) {
      throw std::runtime_error("invalid json syntax");
    }
    ++i;
  }

  bool Match(const char *token) {
    SkipWs();
    size_t n = SDL_strlen(token);
    if (s.compare(i, n, token) == 0) {
      i += n;
      return true;
    }
    return false;
  }

  std::string ParseString() {
    SkipWs();
    if (i >= s.size() || s[i] != '"') {
      throw std::runtime_error("expected string");
    }
    ++i;
    std::string out;
    while (i < s.size()) {
      char ch = s[i++];
      if (ch == '"')
        return out;
      if (ch == '\\') {
        if (i >= s.size())
          throw std::runtime_error("invalid string escape");
        char esc = s[i++];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
          out.push_back(esc);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (i + 4 > s.size())
            throw std::runtime_error("invalid unicode escape");
          std::string hex = s.substr(i, 4);
          i += 4;
          unsigned int code = 0;
          for (char h : hex) {
            code <<= 4;
            if (h >= '0' && h <= '9')
              code |= (h - '0');
            else if (h >= 'a' && h <= 'f')
              code |= (h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
              code |= (h - 'A' + 10);
            else
              throw std::runtime_error("invalid unicode escape");
          }
          if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
          } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default:
          throw std::runtime_error("invalid string escape");
        }
      } else {
        out.push_back(ch);
      }
    }
    throw std::runtime_error("unterminated string");
  }

  lua_Number ParseNumber() {
    SkipWs();
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+'))
      ++i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
      ++i;
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      ++i;
      if (i < s.size() && (s[i] == '-' || s[i] == '+'))
        ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
    }
    if (start == i)
      throw std::runtime_error("invalid number");
    const std::string token = s.substr(start, i - start);
    char *end = nullptr;
    double v = SDL_strtod(token.c_str(), &end);
    if (!end || *end != '\0')
      throw std::runtime_error("invalid number");
    return static_cast<lua_Number>(v);
  }

  void ParseValue(lua_State *L) {
    SkipWs();
    if (i >= s.size())
      throw std::runtime_error("unexpected end of json");

    const char ch = s[i];
    if (ch == '{') {
      ParseObject(L);
    } else if (ch == '[') {
      ParseArray(L);
    } else if (ch == '"') {
      const std::string value = ParseString();
      lua_pushlstring(L, value.c_str(), value.size());
    } else if (ch == 't') {
      if (!Match("true"))
        throw std::runtime_error("invalid token");
      lua_pushboolean(L, 1);
    } else if (ch == 'f') {
      if (!Match("false"))
        throw std::runtime_error("invalid token");
      lua_pushboolean(L, 0);
    } else if (ch == 'n') {
      if (!Match("null"))
        throw std::runtime_error("invalid token");
      lua_pushnil(L);
    } else {
      const lua_Number n = ParseNumber();
      lua_pushnumber(L, n);
    }
  }

  void ParseObject(lua_State *L) {
    Expect('{');
    lua_createtable(L, 0, 8);
    SkipWs();
    if (i < s.size() && s[i] == '}') {
      ++i;
      return;
    }

    while (true) {
      const std::string key = ParseString();
      Expect(':');
      ParseValue(L);
      lua_setfield(L, -2, key.c_str());
      SkipWs();
      if (i < s.size() && s[i] == '}') {
        ++i;
        return;
      }
      Expect(',');
    }
  }

  void ParseArray(lua_State *L) {
    Expect('[');
    lua_createtable(L, 8, 0);
    SkipWs();
    if (i < s.size() && s[i] == ']') {
      ++i;
      return;
    }

    int idx = 1;
    while (true) {
      ParseValue(L);
      lua_rawseti(L, -2, idx++);
      SkipWs();
      if (i < s.size() && s[i] == ']') {
        ++i;
        return;
      }
      Expect(',');
    }
  }
};

VfsFile *VFS::CheckFile(lua_State *L, int idx) {
  return static_cast<VfsFile *>(luaL_checkudata(L, idx, LUA_VFS_FILE_MT));
}

int VFS::L_FileReadAll(lua_State *L) {
  auto *file = CheckFile(L, 1);
  if (!file->io) {
    return luaL_error(L, "vfs file is closed");
  }

  if (SDL_SeekIO(file->io, 0, SDL_IO_SEEK_SET) < 0) {
    return luaL_error(L, "seek failed: %s", SDL_GetError());
  }

  std::string data;
  char buf[4096];
  while (true) {
    const size_t n = SDL_ReadIO(file->io, buf, sizeof(buf));
    if (n == 0)
      break;
    data.append(buf, n);
  }

  lua_pushlstring(L, data.c_str(), data.size());
  return 1;
}

int VFS::L_FileWrite(lua_State *L) {
  auto *file = CheckFile(L, 1);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);
  if (!file->io) {
    return luaL_error(L, "vfs file is closed");
  }

  const size_t written = SDL_WriteIO(file->io, data, len);
  if (written != len) {
    return luaL_error(L, "write failed: %s", SDL_GetError());
  }

  lua_pushinteger(L, static_cast<lua_Integer>(written));
  return 1;
}

int VFS::L_FileClose(lua_State *L) {
  auto *file = CheckFile(L, 1);
  if (file->io) {
    SDL_CloseIO(file->io);
    file->io = nullptr;
  }
  return 0;
}

int VFS::L_Open(lua_State *L) {
  auto *vfs = GetInstance(L);
  const char *path = luaL_checkstring(L, 1);
  const char *mode = luaL_optstring(L, 2, "rb");

  std::filesystem::path resolved;
  try {
    resolved = vfs->ResolveSandboxedPath(path);
  } catch (const std::exception &ex) {
    return luaL_error(L, "vfs.open: %s", ex.what());
  }

  if (mode[0] == 'w' || mode[0] == 'a') {
    std::error_code ec;
    std::filesystem::create_directories(resolved.parent_path(), ec);
  }

  SDL_IOStream *io = SDL_IOFromFile(resolved.string().c_str(), mode);
  if (!io) {
    return luaL_error(L, "vfs.open failed: %s", SDL_GetError());
  }

  auto *file = static_cast<VfsFile *>(lua_newuserdata(L, sizeof(VfsFile)));
  *file = VfsFile{io};
  luaL_getmetatable(L, LUA_VFS_FILE_MT);
  lua_setmetatable(L, -2);
  return 1;
}

int VFS::L_SaveWriteJson(lua_State *L) {
  auto *vfs = GetInstance(L);
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
    json = LuaValueToJson(L, 2);
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

int VFS::L_SaveReadJson(lua_State *L) {
  auto *vfs = GetInstance(L);
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
  std::string json = ss.str();

  try {
    JsonParser parser(json);
    parser.ParseValue(L);
    parser.SkipWs();
    if (parser.i != json.size()) {
      throw std::runtime_error("trailing characters in json");
    }
    return 1;
  } catch (const std::exception &ex) {
    lua_pushnil(L);
    lua_pushstring(L, ex.what());
    return 2;
  }
}

void VFS::RegisterBindings(lua_State *L) {
  SetLuaGlobals(L);

  if (luaL_newmetatable(L, LUA_VFS_FILE_MT)) {
    lua_pushcfunction(L, &VFS::L_FileReadAll);
    lua_setfield(L, -2, "read_all");

    lua_pushcfunction(L, &VFS::L_FileWrite);
    lua_setfield(L, -2, "write");

    lua_pushcfunction(L, &VFS::L_FileClose);
    lua_setfield(L, -2, "close");

    lua_pushcfunction(L, &VFS::L_FileClose);
    lua_setfield(L, -2, "__gc");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);

  lua_newtable(L);
  lua_pushcfunction(L, &VFS::L_Open);
  lua_setfield(L, -2, "open");
  lua_setfield(L, -2, "vfs");

  lua_newtable(L);
  lua_pushcfunction(L, &VFS::L_SaveWriteJson);
  lua_setfield(L, -2, "write_json");

  lua_pushcfunction(L, &VFS::L_SaveReadJson);
  lua_setfield(L, -2, "read_json");

  lua_pushstring(L, GetInstance(L)->SaveRoot().c_str());
  lua_setfield(L, -2, "root");

  lua_setfield(L, -2, "save");
}

} // namespace luna
