#ifndef LUNA_RENDERER_INTERFACE_H
#define LUNA_RENDERER_INTERFACE_H

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "factory.h"
#include "lua_util.hpp"

namespace luna {

namespace asset {
class Vfs;
} // namespace asset

class Renderer {
protected:
  struct PolledEvent {
    std::string type;
    std::optional<std::string> button;
    double x = 0.0;
    double y = 0.0;
  };
  bool InitSdl(SDL_WindowFlags extra_window_flags = 0);
  void FiniSdl();
  void PumpSdlEvents();
  static int L_PollSdlEvents(lua_State *L);
  virtual void ConvertEventCoordinates(double *x, double *y) const {}

  bool sdl_ready_ = false;
  int window_width_ = 1280;
  int window_height_ = 720;
  SDL_Window *window_ = nullptr;
  bool swapchain_dirty_ = false;
  std::vector<PolledEvent> polled_events_;

public:
  virtual ~Renderer() = default;

  virtual bool Init() = 0;
  virtual void ReleaseLua(lua_State *L) = 0;
  virtual void Fini() = 0;
  virtual bool BeginFrame(lua_State *L) = 0;
  virtual bool EndFrame() = 0;
  virtual void BindLua(lua_State *L) = 0;
  virtual bool SetWindowSize(int width, int height) = 0;
  virtual bool HasFatalError() const = 0;
  virtual const std::string &GetFatalError() const = 0;

  virtual std::unique_ptr<AsyncJob> MakeLoadImageJob(asset::Vfs *vfs) = 0;
  virtual std::unique_ptr<AsyncJob> MakeLoadFontfaceJob(asset::Vfs *vfs) = 0;
};

} // namespace luna

#endif
