#ifndef LUNA_RENDERER_INTERFACE_H
#define LUNA_RENDERER_INTERFACE_H

#include <SDL3/SDL.h>

#include <memory>
#include <string>

#include "factory.h"
#include "input_system.h"
#include "lua_util.hpp"

namespace luna {

namespace file {
class Vfs;
} // namespace file

class Renderer {
protected:
  struct AspectFit {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double scale = 1.0;
  };

  bool InitSdl(SDL_WindowFlags extra_window_flags = 0);
  void FiniSdl();
  void PumpSdlEvents();
  InputSystem::CoordinateSpace GetInputCoordinateSpace() const;
  virtual void ConvertEventCoordinates(double *x, double *y) const;
  bool UpdateWindowMetrics(bool *changed = nullptr);
  virtual void UpdateWindowTransform() = 0;
  AspectFit CalculateAspectFit(double outer_width, double outer_height) const;

  bool sdl_ready_ = false;
  // Logical coordinate size used by the window canvas.
  int logical_width_ = 1280;
  int logical_height_ = 720;
  int pixel_width_ = 1280;
  int pixel_height_ = 720;
  AspectFit pixel_viewport_;
  SDL_Window *window_ = nullptr;
  bool swapchain_dirty_ = false;
  InputSystem input_;

public:
  virtual ~Renderer() = default;

  virtual bool Init() = 0;
  virtual void ReleaseLua(lua_State *L) = 0;
  virtual void Fini() = 0;
  virtual bool BeginFrame(lua_State *L) = 0;
  virtual bool EndFrame() = 0;
  virtual void BindLua(lua_State *L) = 0;
  virtual bool SetWindowSize(int width, int height);
  virtual bool HasFatalError() const = 0;
  virtual const std::string &GetFatalError() const = 0;

  virtual std::unique_ptr<AsyncJob> MakeLoadImageJob() = 0;
  virtual std::unique_ptr<AsyncJob> MakeLoadFontfaceJob() = 0;
};

} // namespace luna

#endif
