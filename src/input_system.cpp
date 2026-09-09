#include "input_system.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_touch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>

namespace luna {
namespace {

std::string StableName(const char *name) {
  if (!name || !*name) {
    return {};
  }
  std::string out;
  bool last_separator = false;
  for (const unsigned char ch : std::string(name)) {
    if (std::isalnum(ch)) {
      out.push_back(static_cast<char>(std::tolower(ch)));
      last_separator = false;
    } else if (!last_separator) {
      out.push_back('_');
      last_separator = true;
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::string StableName(std::string_view name) {
  return StableName(std::string(name).c_str());
}

void PushString(lua_State *L, const char *field, const std::string &value) {
  if (!value.empty()) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, field);
  }
}

void PushBool(lua_State *L, const char *field, bool value) {
  lua_pushboolean(L, value);
  lua_setfield(L, -2, field);
}

void PushInteger(lua_State *L, const char *field, lua_Integer value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, field);
}

void PushNumber(lua_State *L, const char *field, lua_Number value) {
  lua_pushnumber(L, value);
  lua_setfield(L, -2, field);
}

void PushStringArray(lua_State *L, const char *field,
    const std::vector<std::string> &values) {
  lua_newtable(L);
  int i = 1;
  for (const std::string &value : values) {
    lua_pushlstring(L, value.data(), value.size());
    lua_rawseti(L, -2, i++);
  }
  lua_setfield(L, -2, field);
}

void PushNumberArray(
    lua_State *L, const char *field, const std::vector<double> &values) {
  lua_newtable(L);
  int i = 1;
  for (const double value : values) {
    lua_pushnumber(L, value);
    lua_rawseti(L, -2, i++);
  }
  lua_setfield(L, -2, field);
}

const char *MouseButtonName(Uint8 button) {
  switch (button) {
  case SDL_BUTTON_LEFT:
    return "left";
  case SDL_BUTTON_MIDDLE:
    return "middle";
  case SDL_BUTTON_RIGHT:
    return "right";
  case SDL_BUTTON_X1:
    return "x1";
  case SDL_BUTTON_X2:
    return "x2";
  default:
    return "";
  }
}

std::optional<Uint8> MouseButtonFromLua(lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const lua_Integer button = lua_tointeger(L, index);
    if (button > 0 && button <= std::numeric_limits<Uint8>::max()) {
      return static_cast<Uint8>(button);
    }
    return std::nullopt;
  }

  const char *name = luaL_checkstring(L, index);
  const std::string stable = StableName(name);
  if (stable == "left") {
    return SDL_BUTTON_LEFT;
  }
  if (stable == "middle") {
    return SDL_BUTTON_MIDDLE;
  }
  if (stable == "right") {
    return SDL_BUTTON_RIGHT;
  }
  if (stable == "x1") {
    return SDL_BUTTON_X1;
  }
  if (stable == "x2") {
    return SDL_BUTTON_X2;
  }
  return std::nullopt;
}

std::string ModifiersName(SDL_Keymod mod) {
  std::vector<std::string> names;
  if (mod & SDL_KMOD_LSHIFT) names.push_back("left_shift");
  if (mod & SDL_KMOD_RSHIFT) names.push_back("right_shift");
  if (mod & SDL_KMOD_LCTRL) names.push_back("left_ctrl");
  if (mod & SDL_KMOD_RCTRL) names.push_back("right_ctrl");
  if (mod & SDL_KMOD_LALT) names.push_back("left_alt");
  if (mod & SDL_KMOD_RALT) names.push_back("right_alt");
  if (mod & SDL_KMOD_LGUI) names.push_back("left_gui");
  if (mod & SDL_KMOD_RGUI) names.push_back("right_gui");
  if (mod & SDL_KMOD_NUM) names.push_back("num_lock");
  if (mod & SDL_KMOD_CAPS) names.push_back("caps_lock");
  if (mod & SDL_KMOD_MODE) names.push_back("mode");
  if (mod & SDL_KMOD_SCROLL) names.push_back("scroll_lock");

  std::string out;
  for (const std::string &name : names) {
    if (!out.empty()) {
      out.push_back('|');
    }
    out += name;
  }
  return out;
}

void PushModifiers(lua_State *L, SDL_Keymod mod) {
  lua_newtable(L);
  PushInteger(L, "mask", static_cast<lua_Integer>(mod));
  PushBool(L, "shift", (mod & SDL_KMOD_SHIFT) != 0);
  PushBool(L, "ctrl", (mod & SDL_KMOD_CTRL) != 0);
  PushBool(L, "alt", (mod & SDL_KMOD_ALT) != 0);
  PushBool(L, "gui", (mod & SDL_KMOD_GUI) != 0);
  PushBool(L, "left_shift", (mod & SDL_KMOD_LSHIFT) != 0);
  PushBool(L, "right_shift", (mod & SDL_KMOD_RSHIFT) != 0);
  PushBool(L, "left_ctrl", (mod & SDL_KMOD_LCTRL) != 0);
  PushBool(L, "right_ctrl", (mod & SDL_KMOD_RCTRL) != 0);
  PushBool(L, "left_alt", (mod & SDL_KMOD_LALT) != 0);
  PushBool(L, "right_alt", (mod & SDL_KMOD_RALT) != 0);
  PushBool(L, "left_gui", (mod & SDL_KMOD_LGUI) != 0);
  PushBool(L, "right_gui", (mod & SDL_KMOD_RGUI) != 0);
  PushBool(L, "num_lock", (mod & SDL_KMOD_NUM) != 0);
  PushBool(L, "caps_lock", (mod & SDL_KMOD_CAPS) != 0);
  PushBool(L, "mode", (mod & SDL_KMOD_MODE) != 0);
  PushBool(L, "scroll_lock", (mod & SDL_KMOD_SCROLL) != 0);
}

std::optional<SDL_Scancode> ScancodeFromLua(lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const lua_Integer scancode = lua_tointeger(L, index);
    if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT) {
      return static_cast<SDL_Scancode>(scancode);
    }
    return std::nullopt;
  }

  const char *name = luaL_checkstring(L, index);
  SDL_Scancode scancode = SDL_GetScancodeFromName(name);
  if (scancode != SDL_SCANCODE_UNKNOWN) {
    return scancode;
  }

  const std::string stable = StableName(name);
  for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
    scancode = static_cast<SDL_Scancode>(i);
    if (StableName(SDL_GetScancodeName(scancode)) == stable) {
      return scancode;
    }
  }
  return std::nullopt;
}

std::string GamepadButtonName(SDL_GamepadButton button) {
  switch (button) {
  case SDL_GAMEPAD_BUTTON_SOUTH:
    return "south";
  case SDL_GAMEPAD_BUTTON_EAST:
    return "east";
  case SDL_GAMEPAD_BUTTON_WEST:
    return "west";
  case SDL_GAMEPAD_BUTTON_NORTH:
    return "north";
  case SDL_GAMEPAD_BUTTON_BACK:
    return "back";
  case SDL_GAMEPAD_BUTTON_GUIDE:
    return "guide";
  case SDL_GAMEPAD_BUTTON_START:
    return "start";
  case SDL_GAMEPAD_BUTTON_LEFT_STICK:
    return "left_stick";
  case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
    return "right_stick";
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
    return "left_shoulder";
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
    return "right_shoulder";
  case SDL_GAMEPAD_BUTTON_DPAD_UP:
    return "dpad_up";
  case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    return "dpad_down";
  case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    return "dpad_left";
  case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
    return "dpad_right";
  case SDL_GAMEPAD_BUTTON_MISC1:
    return "misc1";
  case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
    return "right_paddle1";
  case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
    return "left_paddle1";
  case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
    return "right_paddle2";
  case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
    return "left_paddle2";
  case SDL_GAMEPAD_BUTTON_TOUCHPAD:
    return "touchpad";
  case SDL_GAMEPAD_BUTTON_MISC2:
    return "misc2";
  case SDL_GAMEPAD_BUTTON_MISC3:
    return "misc3";
  case SDL_GAMEPAD_BUTTON_MISC4:
    return "misc4";
  case SDL_GAMEPAD_BUTTON_MISC5:
    return "misc5";
  case SDL_GAMEPAD_BUTTON_MISC6:
    return "misc6";
  default:
    return {};
  }
}

std::optional<SDL_GamepadButton> GamepadButtonFromLua(lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const lua_Integer button = lua_tointeger(L, index);
    if (button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT) {
      return static_cast<SDL_GamepadButton>(button);
    }
    return std::nullopt;
  }

  const std::string stable = StableName(luaL_checkstring(L, index));
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
    const auto button = static_cast<SDL_GamepadButton>(i);
    if (GamepadButtonName(button) == stable ||
        StableName(SDL_GetGamepadStringForButton(button)) == stable) {
      return button;
    }
  }
  return std::nullopt;
}

std::string GamepadAxisName(SDL_GamepadAxis axis) {
  switch (axis) {
  case SDL_GAMEPAD_AXIS_LEFTX:
    return "left_x";
  case SDL_GAMEPAD_AXIS_LEFTY:
    return "left_y";
  case SDL_GAMEPAD_AXIS_RIGHTX:
    return "right_x";
  case SDL_GAMEPAD_AXIS_RIGHTY:
    return "right_y";
  case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
    return "left_trigger";
  case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
    return "right_trigger";
  default:
    return {};
  }
}

std::optional<SDL_GamepadAxis> GamepadAxisFromLua(lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const lua_Integer axis = lua_tointeger(L, index);
    if (axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT) {
      return static_cast<SDL_GamepadAxis>(axis);
    }
    return std::nullopt;
  }

  const std::string stable = StableName(luaL_checkstring(L, index));
  for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
    const auto axis = static_cast<SDL_GamepadAxis>(i);
    const std::string local = GamepadAxisName(axis);
    std::string compact = local;
    compact.erase(std::remove(compact.begin(), compact.end(), '_'),
        compact.end());
    if (local == stable ||
        StableName(SDL_GetGamepadStringForAxis(axis)) == stable ||
        compact == stable) {
      return axis;
    }
  }
  return std::nullopt;
}

std::string GamepadTypeName(SDL_GamepadType type) {
  switch (type) {
  case SDL_GAMEPAD_TYPE_STANDARD:
    return "standard";
  case SDL_GAMEPAD_TYPE_XBOX360:
    return "xbox360";
  case SDL_GAMEPAD_TYPE_XBOXONE:
    return "xboxone";
  case SDL_GAMEPAD_TYPE_PS3:
    return "ps3";
  case SDL_GAMEPAD_TYPE_PS4:
    return "ps4";
  case SDL_GAMEPAD_TYPE_PS5:
    return "ps5";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    return "nintendo_switch_pro";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    return "nintendo_switch_joycon_left";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    return "nintendo_switch_joycon_right";
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    return "nintendo_switch_joycon_pair";
  case SDL_GAMEPAD_TYPE_GAMECUBE:
    return "gamecube";
  default:
    return "unknown";
  }
}

std::string SensorName(SDL_SensorType type) {
  switch (type) {
  case SDL_SENSOR_ACCEL:
    return "accelerometer";
  case SDL_SENSOR_GYRO:
    return "gyroscope";
  case SDL_SENSOR_ACCEL_L:
    return "accelerometer_left";
  case SDL_SENSOR_GYRO_L:
    return "gyroscope_left";
  case SDL_SENSOR_ACCEL_R:
    return "accelerometer_right";
  case SDL_SENSOR_GYRO_R:
    return "gyroscope_right";
  default:
    return "unknown";
  }
}

std::optional<SDL_SensorType> SensorFromLua(lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const lua_Integer sensor = lua_tointeger(L, index);
    if (sensor > SDL_SENSOR_INVALID && sensor < SDL_SENSOR_COUNT) {
      return static_cast<SDL_SensorType>(sensor);
    }
    return std::nullopt;
  }

  const std::string stable = StableName(luaL_checkstring(L, index));
  for (int i = 0; i < SDL_SENSOR_COUNT; ++i) {
    const auto sensor = static_cast<SDL_SensorType>(i);
    if (SensorName(sensor) == stable) {
      return sensor;
    }
  }
  if (stable == "accel") return SDL_SENSOR_ACCEL;
  if (stable == "gyro") return SDL_SENSOR_GYRO;
  return std::nullopt;
}

std::string TouchDeviceTypeName(SDL_TouchDeviceType type) {
  switch (type) {
  case SDL_TOUCH_DEVICE_DIRECT:
    return "direct";
  case SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE:
    return "indirect_absolute";
  case SDL_TOUCH_DEVICE_INDIRECT_RELATIVE:
    return "indirect_relative";
  default:
    return "invalid";
  }
}

double NormalizeGamepadAxis(SDL_GamepadAxis axis, Sint16 value) {
  if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
      axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    return static_cast<double>(std::max<Sint16>(0, value)) / 32767.0;
  }
  if (value < 0) {
    return static_cast<double>(value) / 32768.0;
  }
  return static_cast<double>(value) / 32767.0;
}

InputSystem *CheckInput(lua_State *L) {
  return static_cast<InputSystem *>(lua_touserdata(L, lua_upvalueindex(1)));
}

SDL_JoystickID CheckJoystickId(lua_State *L, int index) {
  return static_cast<SDL_JoystickID>(luaL_checkinteger(L, index));
}

} // namespace

InputSystem::InputSystem() = default;

InputSystem::~InputSystem() { CloseGamepads(); }

void InputSystem::SetWindow(SDL_Window *window) { window_ = window; }

void InputSystem::RefreshGamepads() {
  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  if (!ids) {
    return;
  }

  for (int i = 0; i < count; ++i) {
    OpenGamepad(ids[i]);
  }
  SDL_free(ids);
}

void InputSystem::CloseGamepads() {
  for (auto &[id, state] : gamepads_) {
    (void)id;
    if (state.handle) {
      SDL_CloseGamepad(state.handle);
    }
  }
  gamepads_.clear();
}

void InputSystem::OpenGamepad(SDL_JoystickID id) {
  if (id == 0 || gamepads_.contains(id) || !SDL_IsGamepad(id)) {
    return;
  }

  SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
  if (gamepad) {
    gamepads_.emplace(id, GamepadState{gamepad});
  }
}

void InputSystem::RemoveGamepad(SDL_JoystickID id) {
  auto it = gamepads_.find(id);
  if (it == gamepads_.end()) {
    return;
  }
  if (it->second.handle) {
    SDL_CloseGamepad(it->second.handle);
  }
  gamepads_.erase(it);
}

SDL_Gamepad *InputSystem::FindGamepad(SDL_JoystickID id) const {
  auto it = gamepads_.find(id);
  return it == gamepads_.end() ? nullptr : it->second.handle;
}

bool InputSystem::ConvertWindowPoint(
    const CoordinateSpace &space, double *x, double *y) const {
  if (!x || !y || !space.window || space.logical_width <= 0 ||
      space.logical_height <= 0 || space.pixel_width <= 0 ||
      space.pixel_height <= 0 || space.viewport_scale <= 0.0) {
    return false;
  }

  float pixel_density = SDL_GetWindowPixelDensity(space.window);
  if (pixel_density <= 0.0f) {
    pixel_density = 1.0f;
  }

  const double pixel_x = *x * static_cast<double>(pixel_density);
  const double pixel_y = *y * static_cast<double>(pixel_density);
  *x = (pixel_x - space.viewport_x) / space.viewport_scale;
  *y = (pixel_y - space.viewport_y) / space.viewport_scale;
  return true;
}

void InputSystem::HandleEvent(
    const SDL_Event &event, const CoordinateSpace &space) {
  current_space_ = space;
  InputEvent out;
  out.timestamp_ns = event.common.timestamp;

  switch (event.type) {
  case SDL_EVENT_QUIT:
    out.type = "quit";
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
    out.type = event.type == SDL_EVENT_KEY_DOWN ? "key_down" : "key_up";
    out.has_window_id = event.key.windowID != 0;
    out.window_id = event.key.windowID;
    out.has_keyboard_id = true;
    out.keyboard_id = event.key.which;
    out.scancode = StableName(SDL_GetScancodeName(event.key.scancode));
    out.key = StableName(SDL_GetKeyName(event.key.key));
    out.raw = event.key.raw;
    out.modifiers = event.key.mod;
    out.down = event.key.down;
    out.repeat = event.key.repeat;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_TEXT_INPUT:
    out.type = "text_input";
    out.has_window_id = event.text.windowID != 0;
    out.window_id = event.text.windowID;
    out.text = event.text.text ? event.text.text : "";
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_TEXT_EDITING:
    out.type = "text_editing";
    out.has_window_id = event.edit.windowID != 0;
    out.window_id = event.edit.windowID;
    out.text = event.edit.text ? event.edit.text : "";
    out.start = event.edit.start;
    out.length = event.edit.length;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_TEXT_EDITING_CANDIDATES:
    out.type = "text_editing_candidates";
    out.has_window_id = event.edit_candidates.windowID != 0;
    out.window_id = event.edit_candidates.windowID;
    out.selected_candidate = event.edit_candidates.selected_candidate;
    out.horizontal = event.edit_candidates.horizontal;
    for (Sint32 i = 0; i < event.edit_candidates.num_candidates; ++i) {
      const char *candidate = event.edit_candidates.candidates[i];
      out.candidates.emplace_back(candidate ? candidate : "");
    }
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_KEYMAP_CHANGED:
    out.type = "keymap_changed";
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_KEYBOARD_ADDED:
  case SDL_EVENT_KEYBOARD_REMOVED:
    out.type =
        event.type == SDL_EVENT_KEYBOARD_ADDED ? "keyboard_added"
                                               : "keyboard_removed";
    out.has_keyboard_id = true;
    out.keyboard_id = event.kdevice.which;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_MOUSE_ADDED:
  case SDL_EVENT_MOUSE_REMOVED:
    out.type =
        event.type == SDL_EVENT_MOUSE_ADDED ? "mouse_added" : "mouse_removed";
    out.has_mouse_id = true;
    out.mouse_id = event.mdevice.which;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_WINDOW_MOUSE_ENTER:
  case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    out.type =
        event.type == SDL_EVENT_WINDOW_MOUSE_ENTER ? "mouse_enter"
                                                   : "mouse_leave";
    out.has_window_id = event.window.windowID != 0;
    out.window_id = event.window.windowID;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_MOUSE_MOTION: {
    out.type = "mouse_motion";
    out.has_window_id = event.motion.windowID != 0;
    out.window_id = event.motion.windowID;
    out.has_mouse_id = true;
    out.mouse_id = event.motion.which;
    out.state = event.motion.state;
    out.raw_x = event.motion.x;
    out.raw_y = event.motion.y;
    out.x = event.motion.x;
    out.y = event.motion.y;
    out.dx = event.motion.xrel;
    out.dy = event.motion.yrel;
    ConvertWindowPoint(space, &out.x, &out.y);
    out.has_position = true;
    out.has_delta = true;
    out.in_bounds = out.x >= 0.0 && out.y >= 0.0 &&
                    out.x <= static_cast<double>(space.logical_width) &&
                    out.y <= static_cast<double>(space.logical_height);
    events_.push_back(std::move(out));
    break;
  }
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    out.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                   ? "mouse_button_down"
                   : "mouse_button_up";
    out.has_window_id = event.button.windowID != 0;
    out.window_id = event.button.windowID;
    out.has_mouse_id = true;
    out.mouse_id = event.button.which;
    out.button = MouseButtonName(event.button.button);
    out.down = event.button.down;
    out.clicks = event.button.clicks;
    out.raw_x = event.button.x;
    out.raw_y = event.button.y;
    out.x = event.button.x;
    out.y = event.button.y;
    ConvertWindowPoint(space, &out.x, &out.y);
    out.has_position = true;
    out.in_bounds = out.x >= 0.0 && out.y >= 0.0 &&
                    out.x <= static_cast<double>(space.logical_width) &&
                    out.y <= static_cast<double>(space.logical_height);
    events_.push_back(std::move(out));
    break;
  }
  case SDL_EVENT_MOUSE_WHEEL:
    out.type = "mouse_wheel";
    out.has_window_id = event.wheel.windowID != 0;
    out.window_id = event.wheel.windowID;
    out.has_mouse_id = true;
    out.mouse_id = event.wheel.which;
    out.wheel_x = event.wheel.x;
    out.wheel_y = event.wheel.y;
    out.integer_x = event.wheel.integer_x;
    out.integer_y = event.wheel.integer_y;
    out.wheel_direction =
        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? "flipped"
                                                        : "normal";
    out.raw_x = event.wheel.mouse_x;
    out.raw_y = event.wheel.mouse_y;
    out.x = event.wheel.mouse_x;
    out.y = event.wheel.mouse_y;
    ConvertWindowPoint(space, &out.x, &out.y);
    out.has_position = true;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_CANCELED: {
    if (event.type == SDL_EVENT_FINGER_DOWN) {
      out.type = "finger_down";
    } else if (event.type == SDL_EVENT_FINGER_UP) {
      out.type = "finger_up";
    } else if (event.type == SDL_EVENT_FINGER_MOTION) {
      out.type = "finger_motion";
    } else {
      out.type = "finger_canceled";
    }
    out.has_window_id = event.tfinger.windowID != 0;
    out.window_id = event.tfinger.windowID;
    out.has_touch_id = true;
    out.touch_id = event.tfinger.touchID;
    out.has_finger_id = true;
    out.finger_id = event.tfinger.fingerID;
    out.normalized_x = event.tfinger.x;
    out.normalized_y = event.tfinger.y;
    out.dx = event.tfinger.dx;
    out.dy = event.tfinger.dy;
    out.pressure = event.tfinger.pressure;
    out.has_normalized_position = true;
    out.has_delta = true;
    out.has_pressure = true;
    out.x = out.normalized_x * static_cast<double>(space.logical_width);
    out.y = out.normalized_y * static_cast<double>(space.logical_height);
    out.has_position = space.logical_width > 0 && space.logical_height > 0;
    events_.push_back(std::move(out));
    break;
  }
  case SDL_EVENT_GAMEPAD_ADDED:
    OpenGamepad(event.gdevice.which);
    out.type = "gamepad_added";
    out.has_gamepad_id = true;
    out.gamepad_id = event.gdevice.which;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_GAMEPAD_REMOVED:
    out.type = "gamepad_removed";
    out.has_gamepad_id = true;
    out.gamepad_id = event.gdevice.which;
    events_.push_back(std::move(out));
    RemoveGamepad(event.gdevice.which);
    break;
  case SDL_EVENT_GAMEPAD_REMAPPED:
    out.type = "gamepad_remapped";
    out.has_gamepad_id = true;
    out.gamepad_id = event.gdevice.which;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
    const auto axis = static_cast<SDL_GamepadAxis>(event.gaxis.axis);
    out.type = "gamepad_axis";
    out.has_gamepad_id = true;
    out.gamepad_id = event.gaxis.which;
    out.axis = GamepadAxisName(axis);
    out.value = event.gaxis.value;
    out.normalized_value = NormalizeGamepadAxis(axis, event.gaxis.value);
    events_.push_back(std::move(out));
    break;
  }
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  case SDL_EVENT_GAMEPAD_BUTTON_UP:
    out.type = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
                   ? "gamepad_button_down"
                   : "gamepad_button_up";
    out.has_gamepad_id = true;
    out.gamepad_id = event.gbutton.which;
    out.button =
        GamepadButtonName(static_cast<SDL_GamepadButton>(event.gbutton.button));
    out.down = event.gbutton.down;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
  case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
  case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
      out.type = "gamepad_touchpad_down";
    } else if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION) {
      out.type = "gamepad_touchpad_motion";
    } else {
      out.type = "gamepad_touchpad_up";
    }
    out.has_gamepad_id = true;
    out.gamepad_id = event.gtouchpad.which;
    out.touchpad = event.gtouchpad.touchpad;
    out.finger = event.gtouchpad.finger;
    out.normalized_x = event.gtouchpad.x;
    out.normalized_y = event.gtouchpad.y;
    out.pressure = event.gtouchpad.pressure;
    out.has_normalized_position = true;
    out.has_pressure = true;
    events_.push_back(std::move(out));
    break;
  case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
    out.type = "gamepad_sensor";
    out.has_gamepad_id = true;
    out.gamepad_id = event.gsensor.which;
    out.sensor = SensorName(static_cast<SDL_SensorType>(event.gsensor.sensor));
    out.sensor_values = {event.gsensor.data[0], event.gsensor.data[1],
        event.gsensor.data[2]};
    events_.push_back(std::move(out));
    break;
  default:
    break;
  }
}

void InputSystem::PushEvent(lua_State *L, const InputEvent &event) const {
  lua_newtable(L);
  PushString(L, "type", event.type);
  PushInteger(L, "timestamp_ns", static_cast<lua_Integer>(event.timestamp_ns));
  if (event.has_window_id) {
    PushInteger(L, "window_id", static_cast<lua_Integer>(event.window_id));
  }
  if (event.has_keyboard_id) {
    PushInteger(
        L, "keyboard_id", static_cast<lua_Integer>(event.keyboard_id));
  }
  if (event.has_mouse_id) {
    PushInteger(L, "mouse_id", static_cast<lua_Integer>(event.mouse_id));
  }
  if (event.has_touch_id) {
    PushInteger(L, "touch_id", static_cast<lua_Integer>(event.touch_id));
  }
  if (event.has_finger_id) {
    PushInteger(L, "finger_id", static_cast<lua_Integer>(event.finger_id));
  }
  if (event.has_gamepad_id) {
    PushInteger(
        L, "gamepad_id", static_cast<lua_Integer>(event.gamepad_id));
  }
  PushString(L, "button", event.button);
  PushString(L, "axis", event.axis);
  PushString(L, "scancode", event.scancode);
  PushString(L, "key", event.key);
  PushString(L, "text", event.text);
  PushString(L, "sensor", event.sensor);
  PushString(L, "wheel_direction", event.wheel_direction);
  PushString(L, "touch_device_type", event.touch_device_type);
  if (!event.candidates.empty()) {
    PushStringArray(L, "candidates", event.candidates);
    PushInteger(L, "selected_candidate", event.selected_candidate);
    PushBool(L, "horizontal", event.horizontal);
  }
  if (!event.sensor_values.empty()) {
    PushNumberArray(L, "values", event.sensor_values);
  }
  if (event.start != 0 || event.length != 0 || event.type == "text_editing") {
    PushInteger(L, "start", event.start);
    PushInteger(L, "length", event.length);
  }
  if (event.has_position) {
    PushNumber(L, "x", event.x);
    PushNumber(L, "y", event.y);
    PushNumber(L, "raw_x", event.raw_x);
    PushNumber(L, "raw_y", event.raw_y);
    PushBool(L, "in_bounds", event.in_bounds);
  }
  if (event.has_delta) {
    PushNumber(L, "dx", event.dx);
    PushNumber(L, "dy", event.dy);
  }
  if (event.has_normalized_position) {
    PushNumber(L, "normalized_x", event.normalized_x);
    PushNumber(L, "normalized_y", event.normalized_y);
  }
  if (event.has_pressure) {
    PushNumber(L, "pressure", event.pressure);
  }
  if (event.type == "mouse_wheel") {
    PushNumber(L, "wheel_x", event.wheel_x);
    PushNumber(L, "wheel_y", event.wheel_y);
    PushInteger(L, "integer_x", event.integer_x);
    PushInteger(L, "integer_y", event.integer_y);
  }
  if (event.type.rfind("gamepad_touchpad_", 0) == 0) {
    PushInteger(L, "touchpad", event.touchpad);
    PushInteger(L, "finger", event.finger);
  }
  if (event.type == "gamepad_axis") {
    PushInteger(L, "value", event.value);
    PushNumber(L, "normalized_value", event.normalized_value);
  }
  if (event.type == "key_down" || event.type == "key_up") {
    PushInteger(L, "raw", event.raw);
    PushInteger(L, "modifiers_mask", event.modifiers);
    PushString(L, "modifiers", ModifiersName(static_cast<SDL_Keymod>(
                                  event.modifiers)));
    PushBool(L, "down", event.down);
    PushBool(L, "repeat", event.repeat);
  }
  if (event.type == "mouse_button_down" ||
      event.type == "mouse_button_up") {
    PushBool(L, "down", event.down);
    PushInteger(L, "clicks", event.clicks);
  }
  if (event.type == "mouse_motion") {
    PushInteger(L, "state", event.state);
  }
  if (event.type == "gamepad_button_down" ||
      event.type == "gamepad_button_up") {
    PushBool(L, "down", event.down);
  }
}

void InputSystem::PushEvents(lua_State *L) {
  lua_newtable(L);
  int out_i = 1;
  for (const InputEvent &event : events_) {
    PushEvent(L, event);
    lua_rawseti(L, -2, out_i++);
  }
  events_.clear();
}

int InputSystem::L_PollEvents(lua_State *L) {
  CheckInput(L)->PushEvents(L);
  return 1;
}

int InputSystem::L_IsKeyDown(lua_State *L) {
  const auto scancode = ScancodeFromLua(L, 1);
  if (!scancode) {
    lua_pushboolean(L, false);
    return 1;
  }
  int count = 0;
  const bool *state = SDL_GetKeyboardState(&count);
  lua_pushboolean(L, state && *scancode >= 0 && *scancode < count &&
                         state[*scancode]);
  return 1;
}

int InputSystem::L_Modifiers(lua_State *L) {
  PushModifiers(L, SDL_GetModState());
  return 1;
}

int InputSystem::L_StartTextInput(lua_State *L) {
  InputSystem *input = CheckInput(L);
  if (!input->window_) {
    lua_pushboolean(L, false);
    return 1;
  }
  lua_pushboolean(L, SDL_StartTextInput(input->window_));
  return 1;
}

int InputSystem::L_StopTextInput(lua_State *L) {
  InputSystem *input = CheckInput(L);
  if (!input->window_) {
    lua_pushboolean(L, false);
    return 1;
  }
  lua_pushboolean(L, SDL_StopTextInput(input->window_));
  return 1;
}

int InputSystem::L_TextInputActive(lua_State *L) {
  InputSystem *input = CheckInput(L);
  lua_pushboolean(
      L, input->window_ ? SDL_TextInputActive(input->window_) : false);
  return 1;
}

int InputSystem::L_MousePosition(lua_State *L) {
  InputSystem *input = CheckInput(L);
  float x = 0.0f;
  float y = 0.0f;
  const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
  double logical_x = x;
  double logical_y = y;
  input->ConvertWindowPoint(input->current_space_, &logical_x, &logical_y);
  lua_newtable(L);
  PushNumber(L, "x", logical_x);
  PushNumber(L, "y", logical_y);
  PushNumber(L, "raw_x", x);
  PushNumber(L, "raw_y", y);
  PushInteger(L, "buttons_mask", buttons);
  return 1;
}

int InputSystem::L_MouseButtons(lua_State *L) {
  float x = 0.0f;
  float y = 0.0f;
  const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
  lua_newtable(L);
  PushInteger(L, "mask", buttons);
  PushBool(L, "left", (buttons & SDL_BUTTON_LMASK) != 0);
  PushBool(L, "middle", (buttons & SDL_BUTTON_MMASK) != 0);
  PushBool(L, "right", (buttons & SDL_BUTTON_RMASK) != 0);
  PushBool(L, "x1", (buttons & SDL_BUTTON_X1MASK) != 0);
  PushBool(L, "x2", (buttons & SDL_BUTTON_X2MASK) != 0);
  return 1;
}

int InputSystem::L_IsMouseButtonDown(lua_State *L) {
  const auto button = MouseButtonFromLua(L, 1);
  if (!button) {
    lua_pushboolean(L, false);
    return 1;
  }
  float x = 0.0f;
  float y = 0.0f;
  const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
  lua_pushboolean(L, (buttons & SDL_BUTTON_MASK(*button)) != 0);
  return 1;
}

int InputSystem::L_SetRelativeMouseMode(lua_State *L) {
  InputSystem *input = CheckInput(L);
  const bool enabled = lua_toboolean(L, 1);
  if (!input->window_) {
    lua_pushboolean(L, false);
    return 1;
  }
  const bool ok = SDL_SetWindowRelativeMouseMode(input->window_, enabled);
  if (ok) {
    input->relative_mouse_mode_ = enabled;
  }
  lua_pushboolean(L, ok);
  return 1;
}

int InputSystem::L_RelativeMouseMode(lua_State *L) {
  lua_pushboolean(L, CheckInput(L)->relative_mouse_mode_);
  return 1;
}

int InputSystem::L_ShowCursor(lua_State *L) {
  const bool show = lua_gettop(L) < 1 || lua_toboolean(L, 1);
  lua_pushboolean(L, show ? SDL_ShowCursor() : SDL_HideCursor());
  return 1;
}

int InputSystem::L_CaptureMouse(lua_State *L) {
  lua_pushboolean(L, SDL_CaptureMouse(lua_toboolean(L, 1)));
  return 1;
}

int InputSystem::L_TouchDevices(lua_State *L) {
  int count = 0;
  SDL_TouchID *ids = SDL_GetTouchDevices(&count);
  lua_newtable(L);
  if (!ids) {
    return 1;
  }
  for (int i = 0; i < count; ++i) {
    lua_newtable(L);
    PushInteger(L, "id", static_cast<lua_Integer>(ids[i]));
    const char *name = SDL_GetTouchDeviceName(ids[i]);
    if (name) {
      lua_pushstring(L, name);
      lua_setfield(L, -2, "name");
    }
    PushString(
        L, "type", TouchDeviceTypeName(SDL_GetTouchDeviceType(ids[i])));
    lua_rawseti(L, -2, i + 1);
  }
  SDL_free(ids);
  return 1;
}

int InputSystem::L_TouchFingers(lua_State *L) {
  const auto touch_id = static_cast<SDL_TouchID>(luaL_checkinteger(L, 1));
  int count = 0;
  SDL_Finger **fingers = SDL_GetTouchFingers(touch_id, &count);
  lua_newtable(L);
  if (!fingers) {
    return 1;
  }
  for (int i = 0; i < count; ++i) {
    SDL_Finger *finger = fingers[i];
    lua_newtable(L);
    PushInteger(L, "id", static_cast<lua_Integer>(finger->id));
    PushNumber(L, "normalized_x", finger->x);
    PushNumber(L, "normalized_y", finger->y);
    PushNumber(L, "pressure", finger->pressure);
    lua_rawseti(L, -2, i + 1);
  }
  SDL_free(fingers);
  return 1;
}

int InputSystem::L_Gamepads(lua_State *L) {
  InputSystem *input = CheckInput(L);
  input->RefreshGamepads();
  lua_newtable(L);
  int out_i = 1;
  for (const auto &[id, state] : input->gamepads_) {
    SDL_Gamepad *gamepad = state.handle;
    if (!gamepad) {
      continue;
    }
    lua_newtable(L);
    PushInteger(L, "id", static_cast<lua_Integer>(id));
    const char *name = SDL_GetGamepadName(gamepad);
    if (name) {
      lua_pushstring(L, name);
      lua_setfield(L, -2, "name");
    }
    PushString(L, "type", GamepadTypeName(SDL_GetGamepadType(gamepad)));
    PushInteger(L, "player_index", SDL_GetGamepadPlayerIndex(gamepad));
    PushInteger(L, "touchpads", SDL_GetNumGamepadTouchpads(gamepad));
    lua_rawseti(L, -2, out_i++);
  }
  return 1;
}

int InputSystem::L_GamepadAxis(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  const auto axis = GamepadAxisFromLua(L, 2);
  if (!gamepad || !axis) {
    lua_pushnil(L);
    return 1;
  }
  const Sint16 value = SDL_GetGamepadAxis(gamepad, *axis);
  lua_newtable(L);
  PushInteger(L, "value", value);
  PushNumber(L, "normalized_value", NormalizeGamepadAxis(*axis, value));
  return 1;
}

int InputSystem::L_GamepadButtonDown(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  const auto button = GamepadButtonFromLua(L, 2);
  lua_pushboolean(
      L, gamepad && button && SDL_GetGamepadButton(gamepad, *button));
  return 1;
}

int InputSystem::L_GamepadButtons(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  lua_newtable(L);
  if (!gamepad) {
    return 1;
  }
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
    const auto button = static_cast<SDL_GamepadButton>(i);
    const std::string name = GamepadButtonName(button);
    if (!name.empty()) {
      lua_pushboolean(L, SDL_GetGamepadButton(gamepad, button));
      lua_setfield(L, -2, name.c_str());
    }
  }
  return 1;
}

int InputSystem::L_RumbleGamepad(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  if (!gamepad) {
    lua_pushboolean(L, false);
    return 1;
  }
  const auto low = static_cast<Uint16>(luaL_checkinteger(L, 2));
  const auto high = static_cast<Uint16>(luaL_checkinteger(L, 3));
  const auto duration = static_cast<Uint32>(luaL_checkinteger(L, 4));
  lua_pushboolean(L, SDL_RumbleGamepad(gamepad, low, high, duration));
  return 1;
}

int InputSystem::L_RumbleGamepadTriggers(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  if (!gamepad) {
    lua_pushboolean(L, false);
    return 1;
  }
  const auto left = static_cast<Uint16>(luaL_checkinteger(L, 2));
  const auto right = static_cast<Uint16>(luaL_checkinteger(L, 3));
  const auto duration = static_cast<Uint32>(luaL_checkinteger(L, 4));
  lua_pushboolean(
      L, SDL_RumbleGamepadTriggers(gamepad, left, right, duration));
  return 1;
}

int InputSystem::L_SetGamepadLed(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  if (!gamepad) {
    lua_pushboolean(L, false);
    return 1;
  }
  const auto r = static_cast<Uint8>(luaL_checkinteger(L, 2));
  const auto g = static_cast<Uint8>(luaL_checkinteger(L, 3));
  const auto b = static_cast<Uint8>(luaL_checkinteger(L, 4));
  lua_pushboolean(L, SDL_SetGamepadLED(gamepad, r, g, b));
  return 1;
}

int InputSystem::L_SetGamepadSensorEnabled(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  const auto sensor = SensorFromLua(L, 2);
  if (!gamepad || !sensor) {
    lua_pushboolean(L, false);
    return 1;
  }
  lua_pushboolean(
      L, SDL_SetGamepadSensorEnabled(gamepad, *sensor, lua_toboolean(L, 3)));
  return 1;
}

int InputSystem::L_GamepadSensorData(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  const auto sensor = SensorFromLua(L, 2);
  lua_newtable(L);
  if (!gamepad || !sensor) {
    return 1;
  }
  float values[3] = {};
  if (!SDL_GetGamepadSensorData(gamepad, *sensor, values, 3)) {
    return 1;
  }
  for (int i = 0; i < 3; ++i) {
    lua_pushnumber(L, values[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

int InputSystem::L_GamepadTouchpads(lua_State *L) {
  SDL_Gamepad *gamepad = CheckInput(L)->FindGamepad(CheckJoystickId(L, 1));
  lua_newtable(L);
  if (!gamepad) {
    return 1;
  }
  const int touchpad_count = SDL_GetNumGamepadTouchpads(gamepad);
  for (int touchpad = 0; touchpad < touchpad_count; ++touchpad) {
    lua_newtable(L);
    const int finger_count =
        SDL_GetNumGamepadTouchpadFingers(gamepad, touchpad);
    PushInteger(L, "finger_count", finger_count);
    lua_newtable(L);
    for (int finger = 0; finger < finger_count; ++finger) {
      bool down = false;
      float x = 0.0f;
      float y = 0.0f;
      float pressure = 0.0f;
      SDL_GetGamepadTouchpadFinger(
          gamepad, touchpad, finger, &down, &x, &y, &pressure);
      lua_newtable(L);
      PushBool(L, "down", down);
      PushNumber(L, "normalized_x", x);
      PushNumber(L, "normalized_y", y);
      PushNumber(L, "pressure", pressure);
      lua_rawseti(L, -2, finger + 1);
    }
    lua_setfield(L, -2, "fingers");
    lua_rawseti(L, -2, touchpad + 1);
  }
  return 1;
}

void InputSystem::BindLua(lua_State *L) {
  lua_newtable(L);

  struct Entry {
    const char *name;
    lua_CFunction fn;
  };
  constexpr std::array entries{
      Entry{"poll_events", &InputSystem::L_PollEvents},
      Entry{"is_key_down", &InputSystem::L_IsKeyDown},
      Entry{"modifiers", &InputSystem::L_Modifiers},
      Entry{"start_text_input", &InputSystem::L_StartTextInput},
      Entry{"stop_text_input", &InputSystem::L_StopTextInput},
      Entry{"text_input_active", &InputSystem::L_TextInputActive},
      Entry{"mouse_position", &InputSystem::L_MousePosition},
      Entry{"mouse_buttons", &InputSystem::L_MouseButtons},
      Entry{"is_mouse_button_down", &InputSystem::L_IsMouseButtonDown},
      Entry{"set_relative_mouse_mode", &InputSystem::L_SetRelativeMouseMode},
      Entry{"relative_mouse_mode", &InputSystem::L_RelativeMouseMode},
      Entry{"show_cursor", &InputSystem::L_ShowCursor},
      Entry{"capture_mouse", &InputSystem::L_CaptureMouse},
      Entry{"touch_devices", &InputSystem::L_TouchDevices},
      Entry{"touch_fingers", &InputSystem::L_TouchFingers},
      Entry{"gamepads", &InputSystem::L_Gamepads},
      Entry{"gamepad_axis", &InputSystem::L_GamepadAxis},
      Entry{"gamepad_button_down", &InputSystem::L_GamepadButtonDown},
      Entry{"gamepad_buttons", &InputSystem::L_GamepadButtons},
      Entry{"rumble_gamepad", &InputSystem::L_RumbleGamepad},
      Entry{"rumble_gamepad_triggers", &InputSystem::L_RumbleGamepadTriggers},
      Entry{"set_gamepad_led", &InputSystem::L_SetGamepadLed},
      Entry{"set_gamepad_sensor_enabled",
          &InputSystem::L_SetGamepadSensorEnabled},
      Entry{"gamepad_sensor_data", &InputSystem::L_GamepadSensorData},
      Entry{"gamepad_touchpads", &InputSystem::L_GamepadTouchpads},
  };

  for (const Entry &entry : entries) {
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, entry.fn, 1);
    lua_setfield(L, -2, entry.name);
  }
  lua_setfield(L, -2, "input");
}

} // namespace luna
