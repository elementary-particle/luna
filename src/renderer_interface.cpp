#include "renderer_interface.h"

#include <algorithm>

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
      SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | extra_window_flags;
  window_ =
      SDL_CreateWindow("Luna", logical_width_, logical_height_, window_flags);
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

void Renderer::ConvertEventCoordinates(double *x, double *y) const {
  if (!x || !y || !window_ || pixel_width_ <= 0 || pixel_height_ <= 0 ||
      logical_width_ <= 0 || logical_height_ <= 0) {
    return;
  }

  float pixel_density = SDL_GetWindowPixelDensity(window_);
  if (pixel_density <= 0.0f) {
    pixel_density = 1.0f;
  }

  const double pixel_x = *x * static_cast<double>(pixel_density);
  const double pixel_y = *y * static_cast<double>(pixel_density);
  *x = (pixel_x - pixel_viewport_.x) / pixel_viewport_.scale;
  *y = (pixel_y - pixel_viewport_.y) / pixel_viewport_.scale;
}

Renderer::AspectFit Renderer::CalculateAspectFit(
    double outer_width, double outer_height) const {
  if (outer_width <= 0.0 || outer_height <= 0.0 || logical_width_ <= 0 ||
      logical_height_ <= 0) {
    return {};
  }

  const double scale = std::min(
      outer_width / static_cast<double>(logical_width_),
      outer_height / static_cast<double>(logical_height_));
  const double width = static_cast<double>(logical_width_) * scale;
  const double height = static_cast<double>(logical_height_) * scale;
  return AspectFit{(outer_width - width) * 0.5, (outer_height - height) * 0.5,
      width, height, scale};
}

bool Renderer::UpdateWindowMetrics(bool *changed) {
  if (changed) {
    *changed = false;
  }
  if (!window_) {
    return false;
  }

  int pixel_width = 0;
  int pixel_height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height)) {
    log::Error(
        "renderer", "SDL_GetWindowSizeInPixels failed: {}", SDL_GetError());
    return false;
  }

  if (pixel_width <= 0 || pixel_height <= 0) {
    log::Warn("renderer", "ignoring non-positive pixel size {}x{}",
        pixel_width, pixel_height);
    return false;
  }

  const bool metrics_changed =
      pixel_width_ != pixel_width || pixel_height_ != pixel_height;
  pixel_width_ = pixel_width;
  pixel_height_ = pixel_height;
  pixel_viewport_ = CalculateAspectFit(pixel_width_, pixel_height_);
  UpdateWindowTransform();
  if (changed) {
    *changed = metrics_changed;
  }
  return true;
}

bool Renderer::SetWindowSize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  logical_width_ = width;
  logical_height_ = height;

  if (!window_) {
    pixel_viewport_ = CalculateAspectFit(pixel_width_, pixel_height_);
    UpdateWindowTransform();
    return true;
  }

  if (!UpdateWindowMetrics()) {
    return false;
  }

  log::Info("renderer",
      "logical window size set to {}x{} pixels={}x{} scale={:.2f}",
      logical_width_, logical_height_, pixel_width_, pixel_height_,
      pixel_viewport_.scale);
  return true;
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
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
      UpdateWindowMetrics();
      swapchain_dirty_ = true;
      break;
    }
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
