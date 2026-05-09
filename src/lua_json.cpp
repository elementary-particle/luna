#include "lua_json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lua_util.hpp"

namespace luna::lua_json {
namespace {

int LuaAbsIndex(lua_State *L, int idx) {
  if (idx > 0 || idx <= LUA_REGISTRYINDEX) {
    return idx;
  }
  return lua_gettop(L) + idx + 1;
}

bool LuaIsInteger(lua_State *L, int idx) {
  const lua_Number n = lua_tonumber(L, idx);
  const lua_Integer i = lua_tointeger(L, idx);
  return n == static_cast<lua_Number>(i);
}

std::string JsonEscape(const std::string &s) {
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
        std::snprintf(
            buf, sizeof(buf), "\\u%04x", (unsigned int)(unsigned char)ch);
        out += buf;
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return out;
}

std::string EncodeValue(lua_State *L, int idx);

std::string EncodeTable(lua_State *L, int idx) {
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
      if (i > 1) {
        oss << ",";
      }
      lua_rawgeti(L, idx, static_cast<int>(i));
      oss << EncodeValue(L, -1);
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
    if (!first) {
      oss << ",";
    }
    first = false;
    oss << '"' << JsonEscape(k) << "\":" << EncodeValue(L, -1);
    lua_pop(L, 1);
  }
  oss << "}";
  return oss.str();
}

std::string EncodeValue(lua_State *L, int idx) {
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
    if (std::isnan(n)) {
      throw std::runtime_error("json does not support NaN");
    }
    if (std::isinf(n)) {
      throw std::runtime_error("json does not support infinity");
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", (double)n);
    return buf;
  }
  case LUA_TSTRING:
    return '"' + JsonEscape(lua_tostring(L, idx)) + '"';
  case LUA_TTABLE:
    return EncodeTable(L, idx);
  default:
    throw std::runtime_error("unsupported lua type for json serialization");
  }
}

struct Parser {
  std::string_view s;
  size_t i = 0;

  explicit Parser(std::string_view src) : s(src) {}

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
    size_t n = std::strlen(token);
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
      if (ch == '"') {
        return out;
      }
      if (ch == '\\') {
        if (i >= s.size()) {
          throw std::runtime_error("invalid string escape");
        }
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
          if (i + 4 > s.size()) {
            throw std::runtime_error("invalid unicode escape");
          }
          std::string hex(s.substr(i, 4));
          i += 4;
          unsigned int code = 0;
          for (char h : hex) {
            code <<= 4;
            if (h >= '0' && h <= '9') {
              code |= (h - '0');
            } else if (h >= 'a' && h <= 'f') {
              code |= (h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              code |= (h - 'A' + 10);
            } else {
              throw std::runtime_error("invalid unicode escape");
            }
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
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
      ++i;
    }
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      ++i;
    }
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
      }
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      ++i;
      if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        ++i;
      }
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
      }
    }
    if (start == i) {
      throw std::runtime_error("invalid number");
    }
    const std::string token(s.substr(start, i - start));
    char *end = nullptr;
    double v = std::strtod(token.c_str(), &end);
    if (!end || *end != '\0') {
      throw std::runtime_error("invalid number");
    }
    return static_cast<lua_Number>(v);
  }

  void ParseValue(lua_State *L) {
    SkipWs();
    if (i >= s.size()) {
      throw std::runtime_error("unexpected end of json");
    }

    const char ch = s[i];
    if (ch == '{') {
      ParseObject(L);
    } else if (ch == '[') {
      ParseArray(L);
    } else if (ch == '"') {
      const std::string value = ParseString();
      lua_pushlstring(L, value.c_str(), value.size());
    } else if (ch == 't') {
      if (!Match("true")) {
        throw std::runtime_error("invalid token");
      }
      lua_pushboolean(L, 1);
    } else if (ch == 'f') {
      if (!Match("false")) {
        throw std::runtime_error("invalid token");
      }
      lua_pushboolean(L, 0);
    } else if (ch == 'n') {
      if (!Match("null")) {
        throw std::runtime_error("invalid token");
      }
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

} // namespace

std::string Encode(lua_State *L, int idx) { return EncodeValue(L, idx); }

void Decode(lua_State *L, std::string_view json) {
  Parser parser(json);
  parser.ParseValue(L);
  parser.SkipWs();
  if (parser.i != json.size()) {
    throw std::runtime_error("trailing characters in json");
  }
}

} // namespace luna::lua_json
