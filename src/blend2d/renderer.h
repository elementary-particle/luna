#ifndef LUNA_BLEND2D_RENDERER_H
#define LUNA_BLEND2D_RENDERER_H

#include "vfs.h"

#include <SDL3/SDL.h>

#include <memory>
#include <string>

#include <blend2d/blend2d.h>

#include "blend2d/canvas.h"
#include "factory.h"
#include "file_vfs.h"
#include "renderer_interface.h"

struct lua_State;

namespace luna::backend::blend2d {

class Blend2dRenderer final : public Renderer {
private:
  class LoadImageJob : public AsyncJob {
  public:
    LoadImageJob() = default;
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    AssetInput input_;
    std::string path_;
    file::MappedFile mapping_;
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
    AssetInput input_;
    std::string path_;
    file::MappedFile mapping_;
  };

  void CreatePresentationResources();
  void DestroyPresentationResources();
  void UpdateWindowTransform() override;
  bool EnsureGraphicsReady();
  void RecreatePresentationResources();
  bool LockFramebufferTexture();
  void UnlockFramebufferTexture();
  void DiscardWindowCanvasFramebuffer();
  void SetFatalError(std::string message);

  static int L_MakeCanvas(lua_State *L);

  int window_canvas_ref_ = LUA_NOREF;
  lua_State *lua_ = nullptr;

  SDL_Renderer *sdl_renderer_ = nullptr;
  SDL_Texture *texture_ = nullptr;
  BLImage framebuffer_;
  BLFontManager font_mgr_;
  void *locked_texture_pixels_ = nullptr;
  int locked_texture_pitch_ = 0;
  bool clear_locked_texture_ = false;

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
  void ReleaseLua(lua_State *L) override;
  void Fini() override;
  bool BeginFrame(lua_State *L) override;
  bool EndFrame() override;
  void BindLua(lua_State *L) override;
  bool HasFatalError() const override { return fatal_error_; }
  const std::string &GetFatalError() const override {
    return fatal_error_message_;
  }

  std::unique_ptr<AsyncJob> MakeLoadImageJob() override;
  std::unique_ptr<AsyncJob> MakeLoadFontfaceJob() override;
};

} // namespace luna::backend::blend2d

#endif
