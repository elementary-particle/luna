#include "renderer_interface.h"

#include "log.h"

namespace luna {

bool Renderer::InitSdl(SDL_WindowFlags extra_window_flags) {
  log::Info("renderer", "initializing SDL video/audio/events");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
    log::Error("renderer", "SDL_Init failed: {}", SDL_GetError());
    return false;
  }
  sdl_ready_ = true;

  SDL_WindowFlags window_flags =
      SDL_WINDOW_HIGH_PIXEL_DENSITY | extra_window_flags;
  window_ =
      SDL_CreateWindow("Luna", window_width_, window_height_, window_flags);
  if (!window_) {
    log::Error("renderer", "SDL_CreateWindow failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void Renderer::FiniSdl() {
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  if (sdl_ready_) {
    SDL_Quit();
    sdl_ready_ = false;
  }
}

void Renderer::PumpSdlEvents() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
      polled_events_.push_back(PolledEvent{"quit", std::nullopt, 0, 0});
      break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      swapchain_dirty_ = true;
      break;
    case SDL_EVENT_MOUSE_MOTION: {
      double motion_x = static_cast<double>(e.motion.x);
      double motion_y = static_cast<double>(e.motion.y);
      ConvertEventCoordinates(&motion_x, &motion_y);
      polled_events_.push_back(PolledEvent{
          "mouse_move", std::nullopt, motion_x, motion_y});
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      double button_x = static_cast<double>(e.button.x);
      double button_y = static_cast<double>(e.button.y);
      ConvertEventCoordinates(&button_x, &button_y);
      std::optional<std::string> button;
      if (e.button.button == SDL_BUTTON_LEFT) {
        button = "left";
      } else if (e.button.button == SDL_BUTTON_RIGHT) {
        button = "right";
      } else if (e.button.button == SDL_BUTTON_MIDDLE) {
        button = "middle";
      }
      polled_events_.push_back(PolledEvent{
          e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "mouse_down" : "mouse_up",
          button, button_x, button_y});
      break;
    }
    default:
      break;
    }
  }
}

int Renderer::L_PollSdlEvents(lua_State *L) {
  Renderer *r = static_cast<Renderer *>(lua_touserdata(L, lua_upvalueindex(1)));
  lua_newtable(L);
  int out_i = 1;
  for (const PolledEvent &ev : r->polled_events_) {
    lua_newtable(L);
    lua_pushstring(L, ev.type.c_str());
    lua_setfield(L, -2, "type");
    if (ev.button.has_value()) {
      lua_pushstring(L, ev.button->c_str());
      lua_setfield(L, -2, "button");
    }
    lua_pushnumber(L, ev.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, ev.y);
    lua_setfield(L, -2, "y");
    lua_rawseti(L, -2, out_i);
    out_i++;
  }
  r->polled_events_.clear();
  return 1;
}

} // namespace luna
