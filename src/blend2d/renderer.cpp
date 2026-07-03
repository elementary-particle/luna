#include "blend2d/renderer.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include "backend/canvas.h"
#include "blend2d/font_manager.h"
#include "log.h"

namespace luna::backend::blend2d {

namespace {

BLContextCreateInfo MakeContextCreateInfo(uint32_t thread_count) {
  BLContextCreateInfo info{};
  info.flags = thread_count > 0 ? BL_CONTEXT_CREATE_FLAG_FALLBACK_TO_SYNC : 0u;
  info.thread_count = thread_count;
  return info;
}

bool BeginContext(BLContext *ctx, BLImage &image, uint32_t thread_count) {
  if (thread_count == 0) {
    return ctx->begin(image) == BL_SUCCESS;
  }
  const BLContextCreateInfo info = MakeContextCreateInfo(thread_count);
  return ctx->begin(image, info) == BL_SUCCESS;
}

} // namespace

Blend2dRenderer::Blend2dRenderer(uint32_t thread_count)
    : font_mgr_(MakeRuntimeFontManager()), thread_count_(thread_count) {}

void Blend2dRenderer::SetFatalError(std::string message) {
  fatal_error_ = true;
  fatal_error_message_ = std::move(message);
  log::Error("renderer", "fatal: {}", fatal_error_message_);
}

bool Blend2dRenderer::Init() {
  if (!InitSdl()) {
    return false;
  }

  if (!UpdateWindowMetrics()) {
    FiniSdl();
    return false;
  }

  log::Info("renderer",
      "window created logical={}x{} pixels={}x{} scale={:.2f} "
      "threads={}",
      logical_width_, logical_height_, pixel_width_, pixel_height_,
      pixel_viewport_.scale, thread_count_);
  return true;
}

void Blend2dRenderer::CreatePresentationResources() {
  if (!window_) {
    return;
  }
  if (!sdl_renderer_) {
    sdl_renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!sdl_renderer_) {
      throw std::runtime_error(fmt::format(
          "SDL_CreateRenderer failed: {}", SDL_GetError()));
    }
    if (!SDL_SetRenderVSync(sdl_renderer_, 1)) {
      log::Warn(
          "renderer", "SDL_SetRenderVSync failed: {}", SDL_GetError());
    }
  }

  if (texture_) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  framebuffer_.reset();
  locked_texture_pixels_ = nullptr;
  locked_texture_pitch_ = 0;

  texture_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_STREAMING, pixel_width_, pixel_height_);
  if (!texture_) {
    throw std::runtime_error(fmt::format(
        "SDL_CreateTexture failed: {}", SDL_GetError()));
  }
  clear_locked_texture_ = true;
}

void Blend2dRenderer::DestroyPresentationResources() {
  UnlockFramebufferTexture();
  framebuffer_.reset();
  if (texture_) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  if (sdl_renderer_) {
    SDL_DestroyRenderer(sdl_renderer_);
    sdl_renderer_ = nullptr;
  }
}

void Blend2dRenderer::UpdateWindowTransform() {
  window_to_surface_transform_ = BLMatrix2D(pixel_viewport_.scale, 0.0, 0.0,
      pixel_viewport_.scale, pixel_viewport_.x, pixel_viewport_.y);
}

bool Blend2dRenderer::EnsureGraphicsReady() {
  if (fatal_error_) {
    return false;
  }
  if (graphics_ready_) {
    return true;
  }

  try {
    CreatePresentationResources();
  } catch (std::exception const &e) {
    SetFatalError(fmt::format("graphics initialization failed: {}", e.what()));
    return false;
  }

  graphics_ready_ = true;
  swapchain_dirty_ = false;
  return true;
}

void Blend2dRenderer::RecreatePresentationResources() {
  if (!UpdateWindowMetrics()) {
    return;
  }

  try {
    CreatePresentationResources();
  } catch (std::exception const &e) {
    SetFatalError(fmt::format("presentation recreation failed: {}", e.what()));
    return;
  }

  swapchain_dirty_ = false;
  log::Info("renderer",
      "recreated presentation resources logical={}x{} pixels={}x{} "
      "scale={:.2f}",
      logical_width_, logical_height_, pixel_width_, pixel_height_,
      pixel_viewport_.scale);
}

bool Blend2dRenderer::LockFramebufferTexture() {
  if (!texture_) {
    return false;
  }
  if (locked_texture_pixels_ != nullptr) {
    return true;
  }
  if (!SDL_LockTexture(
          texture_, nullptr, &locked_texture_pixels_, &locked_texture_pitch_)) {
    SetFatalError(fmt::format("SDL_LockTexture failed: {}", SDL_GetError()));
    return false;
  }
  if (locked_texture_pixels_ == nullptr || locked_texture_pitch_ <= 0) {
    UnlockFramebufferTexture();
    SetFatalError("SDL_LockTexture returned invalid framebuffer memory");
    return false;
  }
  if (clear_locked_texture_) {
    std::memset(locked_texture_pixels_, 0,
        static_cast<size_t>(locked_texture_pitch_) *
            static_cast<size_t>(pixel_height_));
    clear_locked_texture_ = false;
  }
  if (framebuffer_.create_from_data(pixel_width_, pixel_height_,
          BL_FORMAT_PRGB32, locked_texture_pixels_, locked_texture_pitch_) !=
      BL_SUCCESS) {
    UnlockFramebufferTexture();
    SetFatalError("failed to create Blend2D framebuffer from locked texture");
    return false;
  }
  return true;
}

void Blend2dRenderer::UnlockFramebufferTexture() {
  framebuffer_.reset();
  if (locked_texture_pixels_ == nullptr) {
    locked_texture_pitch_ = 0;
    return;
  }
  SDL_UnlockTexture(texture_);
  locked_texture_pixels_ = nullptr;
  locked_texture_pitch_ = 0;
}

void Blend2dRenderer::DiscardWindowCanvasFramebuffer() {
  if (!lua_ || window_canvas_ref_ == LUA_NOREF) {
    return;
  }

  lua_rawgeti(lua_, LUA_REGISTRYINDEX, window_canvas_ref_);
  if (!lua_isnil(lua_, -1)) {
    auto *canvas = lua::Check<Canvas>(lua_, -1);
    canvas->TakeTopImage();
  }
  lua_pop(lua_, 1);
}

void Blend2dRenderer::ReleaseLua(lua_State *L) {
  if (L != nullptr && lua_ == L && window_canvas_ref_ != LUA_NOREF) {
    DiscardWindowCanvasFramebuffer();
    luaL_unref(L, LUA_REGISTRYINDEX, window_canvas_ref_);
  }
  window_canvas_ref_ = LUA_NOREF;
  lua_ = nullptr;
}

void Blend2dRenderer::Fini() {
  frame_active_ = false;
  DestroyPresentationResources();
  graphics_ready_ = false;
  FiniSdl();
}

bool Blend2dRenderer::BeginFrame(lua_State *L) {
  ZoneScopedN("BeginFrame");
  frame_active_ = false;
  PumpSdlEvents();

  if (!EnsureGraphicsReady()) {
    return false;
  }

  bool metrics_changed = false;
  if (!UpdateWindowMetrics(&metrics_changed)) {
    return false;
  }
  if (metrics_changed) {
    swapchain_dirty_ = true;
  }
  if (swapchain_dirty_) {
    RecreatePresentationResources();
    if (fatal_error_) {
      return false;
    }
  }
  if (!LockFramebufferTexture()) {
    return false;
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, window_canvas_ref_);
  if (!lua_isnil(L, -1)) {
    auto *canvas = lua::Check<Canvas>(L, -1);
    canvas->Init(std::move(framebuffer_), font_mgr_, window_to_surface_transform_,
        thread_count_);
    canvas->ClipRect(0.0, 0.0, logical_width_, logical_height_);
  }
  lua_pop(L, 1);
  frame_active_ = true;
  return true;
}

bool Blend2dRenderer::EndFrame() {
  ZoneScopedN("EndFrame");
  if (!frame_active_) {
    return !fatal_error_;
  }

  bool took_framebuffer = false;
  lua_rawgeti(lua_, LUA_REGISTRYINDEX, window_canvas_ref_);
  if (!lua_isnil(lua_, -1)) {
    auto *canvas = lua::Check<Canvas>(lua_, -1);
    canvas->Flush();
    framebuffer_ = canvas->TakeTopImage();
    took_framebuffer = true;
  }
  lua_pop(lua_, 1);

  if (!took_framebuffer || framebuffer_.is_empty() ||
      locked_texture_pixels_ == nullptr) {
    SetFatalError("failed to access texture-backed framebuffer");
    frame_active_ = false;
    return false;
  }

  UnlockFramebufferTexture();
  if (!SDL_RenderClear(sdl_renderer_)) {
    SetFatalError(fmt::format("SDL_RenderClear failed: {}", SDL_GetError()));
    frame_active_ = false;
    return false;
  }
  if (!SDL_RenderTexture(sdl_renderer_, texture_, nullptr, nullptr)) {
    SetFatalError(fmt::format("SDL_RenderTexture failed: {}", SDL_GetError()));
    frame_active_ = false;
    return false;
  }
  SDL_RenderPresent(sdl_renderer_);

  frame_active_ = false;
  return true;
}

std::unique_ptr<AsyncJob> Blend2dRenderer::MakeLoadImageJob(asset::Vfs *vfs) {
  return std::make_unique<LoadImageJob>(vfs);
}

void Blend2dRenderer::LoadImageJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!path || !*path) {
    luaL_error(L, "load_image: path is empty");
  }
  path_ = path;
  if (!vfs_) {
    luaL_error(L, "load_image: VFS is not available");
  }
  auto mapped = vfs_->MapFile(path_);
  if (!mapped) {
    luaL_error(L, "load_image: failed to open file: %s", path);
  }
  mapping_ = std::move(mapped).value();
}

void Blend2dRenderer::LoadImageJob::Run() {
  ZoneScopedN("LoadImage");
  if (image_.image.read_from_data(mapping_.data(),
          static_cast<size_t>(mapping_.size())) != BL_SUCCESS ||
      image_.image.is_empty()) {
    error_ = fmt::format("failed to decode image: {}", path_);
  }
}

int Blend2dRenderer::LoadImageJob::Finish(lua_State *L) {
  ZoneScopedN("FinishLoadImage");
  auto *image = lua::New<Image>(L, std::move(image_));
  lua_pushnumber(L, image->image.width());
  lua_pushnumber(L, image->image.height());
  return 3;
}

std::unique_ptr<AsyncJob> Blend2dRenderer::MakeLoadFontfaceJob(asset::Vfs *vfs) {
  return std::make_unique<LoadFontfaceJob>(font_mgr_, vfs);
}

void Blend2dRenderer::LoadFontfaceJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!path || !*path) {
    luaL_error(L, "register_font: path is empty");
  }

  path_ = path;
  if (!vfs_) {
    luaL_error(L, "register_font: VFS is not available");
  }
  auto mapped = vfs_->MapFile(path_);
  if (!mapped) {
    luaL_error(L, "register_font: failed to open file: %s", path);
  }
  mapping_ = std::move(mapped).value();
}

void Blend2dRenderer::LoadFontfaceJob::Run() {
  if (!RegisterRuntimeFont(&font_mgr_, std::move(mapping_))) {
    error_ = fmt::format("failed to register font '{}'", path_);
  }
}

int Blend2dRenderer::LoadFontfaceJob::Finish(lua_State * /*L*/) { return 0; }

int Blend2dRenderer::L_MakeCanvas(lua_State *L) {
  auto *renderer =
      static_cast<Blend2dRenderer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const int width = static_cast<int>(luaL_checkinteger(L, 1));
  const int height = static_cast<int>(luaL_checkinteger(L, 2));

  if (width <= 0 || height <= 0) {
    return luaL_error(L, "make_canvas: width and height must be positive");
  }

  BLImage image(width, height, BL_FORMAT_PRGB32);
  if (image.is_empty()) {
    return luaL_error(L, "make_canvas: failed to create image");
  }
  BLContext ctx;
  if (!BeginContext(&ctx, image, renderer->thread_count_)) {
    return luaL_error(L, "make_canvas: failed to create render context");
  }
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.clear_all();
  ctx.end();

  auto *canvas = lua::New<Canvas>(L);
  canvas->Init(std::move(image), renderer->font_mgr_,
      BLMatrix2D::make_identity(), renderer->thread_count_);
  return 1;
}

void Blend2dRenderer::BindLua(lua_State *L) {
  lua_ = L;

  if (lua::NewType<Image>(L)) {
    lua_pushcfunction(L, [](lua_State *L) {
      auto *image = lua::Check<Image>(L, 1);
      image->image.reset();
      return 0;
    });
    lua_setfield(L, -2, "destroy");
  }
  lua_pop(L, 1);

  LCanvas<Backend>::Bind(L);

  input_.BindLua(L);

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_MakeCanvas, 1);
  lua_setfield(L, -2, "make_canvas");

  lua::New<Canvas>(L);
  lua::Check<Canvas>(L, -1)->font_mgr = font_mgr_;
  lua_pushvalue(L, -1);
  window_canvas_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_setfield(L, -2, "window");
}

} // namespace luna::backend::blend2d
