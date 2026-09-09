#ifndef LUNA_INPUT_SYSTEM_H
#define LUNA_INPUT_SYSTEM_H

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "lua_util.hpp"

namespace luna {

class InputSystem {
public:
  struct CoordinateSpace {
    SDL_Window *window = nullptr;
    int logical_width = 0;
    int logical_height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
    double viewport_x = 0.0;
    double viewport_y = 0.0;
    double viewport_scale = 1.0;
  };

  InputSystem();
  ~InputSystem();

  void SetWindow(SDL_Window *window);
  void RefreshGamepads();
  void CloseGamepads();
  void HandleEvent(const SDL_Event &event, const CoordinateSpace &space);
  void BindLua(lua_State *L);

private:
  struct InputEvent {
    std::string type;
    Uint64 timestamp_ns = 0;
    SDL_WindowID window_id = 0;
    bool has_window_id = false;

    Uint64 keyboard_id = 0;
    Uint64 mouse_id = 0;
    Uint64 touch_id = 0;
    Uint64 finger_id = 0;
    SDL_JoystickID gamepad_id = 0;
    bool has_keyboard_id = false;
    bool has_mouse_id = false;
    bool has_touch_id = false;
    bool has_finger_id = false;
    bool has_gamepad_id = false;

    std::string button;
    std::string axis;
    std::string scancode;
    std::string key;
    std::string sensor;
    std::string wheel_direction;
    std::string touch_device_type;
    std::vector<std::string> candidates;
    std::vector<double> sensor_values;

    std::string text;
    Sint32 start = 0;
    Sint32 length = 0;
    Sint32 selected_candidate = -1;
    bool horizontal = false;

    double x = 0.0;
    double y = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    double raw_x = 0.0;
    double raw_y = 0.0;
    double normalized_x = 0.0;
    double normalized_y = 0.0;
    double pressure = 0.0;
    double wheel_x = 0.0;
    double wheel_y = 0.0;
    int integer_x = 0;
    int integer_y = 0;
    int touchpad = 0;
    int finger = 0;
    Sint16 value = 0;
    double normalized_value = 0.0;

    Uint32 raw = 0;
    Uint32 modifiers = 0;
    Uint32 state = 0;
    Uint8 clicks = 0;
    bool down = false;
    bool repeat = false;
    bool in_bounds = false;
    bool has_position = false;
    bool has_delta = false;
    bool has_normalized_position = false;
    bool has_pressure = false;
  };

  struct GamepadState {
    SDL_Gamepad *handle = nullptr;
  };

  static int L_PollEvents(lua_State *L);
  static int L_IsKeyDown(lua_State *L);
  static int L_Modifiers(lua_State *L);
  static int L_StartTextInput(lua_State *L);
  static int L_StopTextInput(lua_State *L);
  static int L_TextInputActive(lua_State *L);
  static int L_MousePosition(lua_State *L);
  static int L_MouseButtons(lua_State *L);
  static int L_IsMouseButtonDown(lua_State *L);
  static int L_SetRelativeMouseMode(lua_State *L);
  static int L_RelativeMouseMode(lua_State *L);
  static int L_ShowCursor(lua_State *L);
  static int L_CaptureMouse(lua_State *L);
  static int L_TouchDevices(lua_State *L);
  static int L_TouchFingers(lua_State *L);
  static int L_Gamepads(lua_State *L);
  static int L_GamepadAxis(lua_State *L);
  static int L_GamepadButtonDown(lua_State *L);
  static int L_GamepadButtons(lua_State *L);
  static int L_RumbleGamepad(lua_State *L);
  static int L_RumbleGamepadTriggers(lua_State *L);
  static int L_SetGamepadLed(lua_State *L);
  static int L_SetGamepadSensorEnabled(lua_State *L);
  static int L_GamepadSensorData(lua_State *L);
  static int L_GamepadTouchpads(lua_State *L);

  void PushEvents(lua_State *L);
  void PushEvent(lua_State *L, const InputEvent &event) const;
  void OpenGamepad(SDL_JoystickID id);
  void RemoveGamepad(SDL_JoystickID id);
  SDL_Gamepad *FindGamepad(SDL_JoystickID id) const;
  bool ConvertWindowPoint(
      const CoordinateSpace &space, double *x, double *y) const;

  SDL_Window *window_ = nullptr;
  CoordinateSpace current_space_;
  bool relative_mouse_mode_ = false;
  std::vector<InputEvent> events_;
  std::unordered_map<SDL_JoystickID, GamepadState> gamepads_;
};

} // namespace luna

#endif
