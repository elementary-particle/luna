#include "input_system.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <string>

namespace {

struct LuaCloser {
  void operator()(lua_State *L) const {
    if (L) {
      lua_close(L);
    }
  }
};

using LuaState = std::unique_ptr<lua_State, LuaCloser>;

LuaState MakeLuaWithInput(luna::InputSystem *input) {
  LuaState L(luaL_newstate());
  assert(L);
  lua_newtable(L.get());
  input->BindLua(L.get());
  lua_setglobal(L.get(), "luna");
  return L;
}

void PollInput(lua_State *L) {
  lua_getglobal(L, "luna");
  lua_getfield(L, -1, "input");
  lua_getfield(L, -1, "poll_events");
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  lua_remove(L, -2);
  lua_remove(L, -2);
}

void PollLegacy(lua_State *L) {
  lua_getglobal(L, "luna");
  lua_getfield(L, -1, "poll_events");
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  lua_remove(L, -2);
}

std::string FieldString(lua_State *L, int index, const char *field) {
  lua_getfield(L, index, field);
  const char *value = lua_tostring(L, -1);
  std::string out = value ? value : "";
  lua_pop(L, 1);
  return out;
}

lua_Number FieldNumber(lua_State *L, int index, const char *field) {
  lua_getfield(L, index, field);
  lua_Number out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return out;
}

bool FieldBool(lua_State *L, int index, const char *field) {
  lua_getfield(L, index, field);
  bool out = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return out;
}

void TestRichEvents() {
  luna::InputSystem input;
  auto L = MakeLuaWithInput(&input);
  luna::InputSystem::CoordinateSpace space{
      nullptr, 1280, 720, 1280, 720, 0.0, 0.0, 1.0};

  SDL_Event key{};
  key.type = SDL_EVENT_KEY_DOWN;
  key.common.timestamp = 123;
  key.key.windowID = 7;
  key.key.which = 42;
  key.key.scancode = SDL_SCANCODE_A;
  key.key.key = SDLK_A;
  key.key.mod = SDL_KMOD_LSHIFT;
  key.key.raw = 30;
  key.key.down = true;
  key.key.repeat = true;
  input.HandleEvent(key, space);

  SDL_Event mouse{};
  mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  mouse.common.timestamp = 456;
  mouse.button.windowID = 7;
  mouse.button.which = 3;
  mouse.button.button = SDL_BUTTON_LEFT;
  mouse.button.down = true;
  mouse.button.clicks = 2;
  mouse.button.x = 100.0f;
  mouse.button.y = 50.0f;
  input.HandleEvent(mouse, space);

  SDL_Event touch{};
  touch.type = SDL_EVENT_FINGER_MOTION;
  touch.common.timestamp = 789;
  touch.tfinger.windowID = 7;
  touch.tfinger.touchID = 11;
  touch.tfinger.fingerID = 12;
  touch.tfinger.x = 0.5f;
  touch.tfinger.y = 0.25f;
  touch.tfinger.dx = 0.1f;
  touch.tfinger.dy = -0.2f;
  touch.tfinger.pressure = 0.75f;
  input.HandleEvent(touch, space);

  PollInput(L.get());
  assert(lua_objlen(L.get(), -1) == 3);

  lua_rawgeti(L.get(), -1, 1);
  assert(FieldString(L.get(), -1, "type") == "key_down");
  assert(FieldString(L.get(), -1, "scancode") == "a");
  assert(FieldString(L.get(), -1, "key") == "a");
  assert(FieldBool(L.get(), -1, "repeat"));
  lua_pop(L.get(), 1);

  lua_rawgeti(L.get(), -1, 2);
  assert(FieldString(L.get(), -1, "type") == "mouse_button_down");
  assert(FieldString(L.get(), -1, "button") == "left");
  assert(FieldNumber(L.get(), -1, "x") == 100.0);
  assert(FieldNumber(L.get(), -1, "y") == 50.0);
  lua_pop(L.get(), 1);

  lua_rawgeti(L.get(), -1, 3);
  assert(FieldString(L.get(), -1, "type") == "finger_motion");
  assert(FieldNumber(L.get(), -1, "x") == 640.0);
  assert(FieldNumber(L.get(), -1, "y") == 180.0);
  assert(FieldNumber(L.get(), -1, "pressure") > 0.74);
  lua_pop(L.get(), 2);
}

void TestLegacyEvents() {
  luna::InputSystem input;
  auto L = MakeLuaWithInput(&input);
  luna::InputSystem::CoordinateSpace space{
      nullptr, 1280, 720, 1280, 720, 0.0, 0.0, 1.0};

  SDL_Event quit{};
  quit.type = SDL_EVENT_QUIT;
  input.HandleEvent(quit, space);

  SDL_Event mouse{};
  mouse.type = SDL_EVENT_MOUSE_BUTTON_UP;
  mouse.button.button = SDL_BUTTON_LEFT;
  mouse.button.x = 11.0f;
  mouse.button.y = 22.0f;
  input.HandleEvent(mouse, space);

  PollLegacy(L.get());
  assert(lua_objlen(L.get(), -1) == 2);
  lua_rawgeti(L.get(), -1, 1);
  assert(FieldString(L.get(), -1, "type") == "quit");
  lua_pop(L.get(), 1);
  lua_rawgeti(L.get(), -1, 2);
  assert(FieldString(L.get(), -1, "type") == "mouse_up");
  assert(FieldString(L.get(), -1, "button") == "left");
  assert(FieldNumber(L.get(), -1, "x") == 11.0);
  assert(FieldNumber(L.get(), -1, "y") == 22.0);
  lua_pop(L.get(), 2);
}

} // namespace

int main() {
  TestRichEvents();
  TestLegacyEvents();
  return 0;
}
