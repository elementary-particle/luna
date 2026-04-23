#ifndef LUNA_BLEND2D_RENDERER_H
#define LUNA_BLEND2D_RENDERER_H

#include <SDL3/SDL.h>

#include <memory>
#include <string>

#include <blend2d/blend2d.h>

#include "blend2d/canvas.h"
#include "factory.h"
#include "renderer_interface.h"

struct lua_State;

namespace luna::backend::blend2d {

class Blend2dRenderer final : public Renderer {
private:
  class LoadImageJob : public AsyncJob {
  public:
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    std::string path_;
    Image image_;
  };

  class LoadFontfaceJob : public AsyncJob {
  public:
    explicit LoadFontfaceJob(BLFontManager font_mgr)
        : font_mgr_(std::move(font_mgr)) {}
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    BLFontManager font_mgr_;
    std::string path_;
  };

  void CreatePresentationResources();
  void DestroyPresentationResources();
  bool UpdateWindowMetrics(bool *changed = nullptr);
  bool EnsureGraphicsReady();
  void RecreatePresentationResources();
  void SetFatalError(std::string message);

  static int L_MakeCanvas(lua_State *L);

  int window_canvas_ref_ = LUA_NOREF;
  lua_State *lua_ = nullptr;

  SDL_Renderer *sdl_renderer_ = nullptr;
  SDL_Texture *texture_ = nullptr;
  BLImage framebuffer_;
  BLFontManager font_mgr_;

  int canvas_width_ = 1280;
  int canvas_height_ = 720;
  float canvas_scale_x_ = 1.0f;
  float canvas_scale_y_ = 1.0f;
  BLMatrix2D window_to_surface_transform_ = BLMatrix2D::make_identity();
  bool graphics_ready_ = false;
  bool frame_active_ = false;
  bool fatal_error_ = false;
  uint32_t thread_count_ = 0;
  std::string fatal_error_message_;

public:
  explicit Blend2dRenderer(uint32_t thread_count = 0);
  ~Blend2dRenderer() override = default;

  bool Init() override;
  void Fini() override;
  bool BeginFrame(lua_State *L) override;
  bool EndFrame() override;
  void RegisterBindings(lua_State *L) override;
  bool SetWindowSize(int width, int height) override;
  bool HasFatalError() const override { return fatal_error_; }
  const std::string &GetFatalError() const override {
    return fatal_error_message_;
  }

  std::unique_ptr<AsyncJob> MakeLoadImageJob() override;
  std::unique_ptr<AsyncJob> MakeLoadFontfaceJob() override;
};

} // namespace luna::backend::blend2d

#endif
