#ifndef LUNA_LUA_JSON_H
#define LUNA_LUA_JSON_H

#include <string>
#include <string_view>

struct lua_State;

namespace luna::lua_json {

std::string Encode(lua_State *L, int idx);
void Decode(lua_State *L, std::string_view json);

} // namespace luna::lua_json

#endif
