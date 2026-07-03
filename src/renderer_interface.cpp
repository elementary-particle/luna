#include "renderer_interface.h"

#include <algorithm>

#include "log.h"

namespace luna {

bool Renderer::InitSdl(SDL_WindowFlags extra_window_flags) {
  log::Info("renderer", "initializing SDL video/audio/input/events");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
          SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
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
  input_.SetWindow(window_);
  input_.RefreshGamepads();

  return true;
}

void Renderer::FiniSdl() {
  input_.CloseGamepads();
  input_.SetWindow(nullptr);

  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  if (sdl_ready_) {
    SDL_Quit();
    sdl_ready_ = false;
  }
}

InputSystem::CoordinateSpace Renderer::GetInputCoordinateSpace() const {
  return InputSystem::CoordinateSpace{window_, logical_width_, logical_height_,
      pixel_width_, pixel_height_, pixel_viewport_.x, pixel_viewport_.y,
      pixel_viewport_.scale};
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
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
      UpdateWindowMetrics();
      swapchain_dirty_ = true;
      break;
    }
    default:
      break;
    }
    input_.HandleEvent(e, GetInputCoordinateSpace());
  }
}

} // namespace luna
