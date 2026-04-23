#include "blend2d/renderer.h"

#include <algorithm>
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
      "window created size={}x{} canvas={}x{} scale={:.2f}x{:.2f} threads={}",
      window_width_, window_height_, canvas_width_, canvas_height_,
      canvas_scale_x_, canvas_scale_y_, thread_count_);
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
  }

  if (texture_) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  framebuffer_.reset();

  if (framebuffer_.create(canvas_width_, canvas_height_, BL_FORMAT_PRGB32) !=
      BL_SUCCESS) {
    throw std::runtime_error("failed to create Blend2D framebuffer");
  }

  {
    BLContext ctx;
    if (!BeginContext(&ctx, framebuffer_, thread_count_)) {
      throw std::runtime_error("failed to begin Blend2D framebuffer context");
    }
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0u));
    ctx.end();
  }

  texture_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_STREAMING, canvas_width_, canvas_height_);
  if (!texture_) {
    throw std::runtime_error(fmt::format(
        "SDL_CreateTexture failed: {}", SDL_GetError()));
  }
}

void Blend2dRenderer::DestroyPresentationResources() {
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

bool Blend2dRenderer::UpdateWindowMetrics(bool *changed) {
  if (changed) {
    *changed = false;
  }
  if (!window_) {
    return false;
  }

  int window_width = 0;
  int window_height = 0;
  if (!SDL_GetWindowSize(window_, &window_width, &window_height)) {
    log::Error("renderer", "SDL_GetWindowSize failed: {}", SDL_GetError());
    return false;
  }

  int canvas_width = 0;
  int canvas_height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &canvas_width, &canvas_height)) {
    log::Error(
        "renderer", "SDL_GetWindowSizeInPixels failed: {}", SDL_GetError());
    return false;
  }

  if (window_width <= 0 || window_height <= 0 || canvas_width <= 0 ||
      canvas_height <= 0) {
    log::Warn("renderer",
        "ignoring non-positive window/canvas size window={}x{} canvas={}x{}",
        window_width, window_height, canvas_width, canvas_height);
    return false;
  }

  const bool metrics_changed = window_width_ != window_width ||
      window_height_ != window_height || canvas_width_ != canvas_width ||
      canvas_height_ != canvas_height;
  window_width_ = window_width;
  window_height_ = window_height;
  canvas_width_ = canvas_width;
  canvas_height_ = canvas_height;
  canvas_scale_x_ =
      static_cast<float>(canvas_width_) / static_cast<float>(window_width_);
  canvas_scale_y_ =
      static_cast<float>(canvas_height_) / static_cast<float>(window_height_);
  window_to_surface_transform_ =
      BLMatrix2D::make_scaling(canvas_scale_x_, canvas_scale_y_);
  if (changed) {
    *changed = metrics_changed;
  }
  return true;
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
      "recreated presentation resources window={}x{} canvas={}x{} scale={:.2f}x{:.2f}",
      window_width_, window_height_, canvas_width_, canvas_height_,
      canvas_scale_x_, canvas_scale_y_);
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

  lua_rawgeti(L, LUA_REGISTRYINDEX, window_canvas_ref_);
  if (!lua_isnil(L, -1)) {
    auto *canvas = lua::Check<Canvas>(L, -1);
    canvas->Init(std::move(framebuffer_), font_mgr_, window_to_surface_transform_,
        thread_count_);
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

  const BLImage *present_image = nullptr;
  lua_rawgeti(lua_, LUA_REGISTRYINDEX, window_canvas_ref_);
  if (!lua_isnil(lua_, -1)) {
    auto *canvas = lua::Check<Canvas>(lua_, -1);
    canvas->Flush();
    present_image = canvas->CurrentImage();
  }
  lua_pop(lua_, 1);

  if (present_image == nullptr || present_image->is_empty()) {
    SetFatalError("failed to access presented framebuffer");
    frame_active_ = false;
    return false;
  }

  BLImageData data{};
  if (present_image->get_data(&data) != BL_SUCCESS || data.pixel_data == nullptr) {
    SetFatalError("failed to access framebuffer pixels");
    frame_active_ = false;
    return false;
  }

  if (!SDL_UpdateTexture(texture_, nullptr, data.pixel_data,
          static_cast<int>(data.stride))) {
    SetFatalError(fmt::format("SDL_UpdateTexture failed: {}", SDL_GetError()));
    frame_active_ = false;
    return false;
  }
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
  framebuffer_ = *present_image;

  frame_active_ = false;
  return true;
}

std::unique_ptr<AsyncJob> Blend2dRenderer::MakeLoadImageJob() {
  return std::make_unique<LoadImageJob>();
}

void Blend2dRenderer::LoadImageJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!path || !*path) {
    luaL_error(L, "load_image: path is empty");
  }
  path_ = path;
}

void Blend2dRenderer::LoadImageJob::Run() {
  ZoneScopedN("LoadImage");
  if (image_.image.read_from_file(path_.c_str()) != BL_SUCCESS ||
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

std::unique_ptr<AsyncJob> Blend2dRenderer::MakeLoadFontfaceJob() {
  return std::make_unique<LoadFontfaceJob>(font_mgr_);
}

void Blend2dRenderer::LoadFontfaceJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!path || !*path) {
    luaL_error(L, "register_font: path is empty");
  }

  path_ = path;
}

void Blend2dRenderer::LoadFontfaceJob::Run() {
  if (!RegisterRuntimeFont(&font_mgr_, path_.c_str())) {
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

void Blend2dRenderer::RegisterBindings(lua_State *L) {
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

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_PollSdlEvents, 1);
  lua_setfield(L, -2, "poll_events");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_MakeCanvas, 1);
  lua_setfield(L, -2, "make_canvas");

  lua::New<Canvas>(L);
  lua::Check<Canvas>(L, -1)->font_mgr = font_mgr_;
  lua_pushvalue(L, -1);
  window_canvas_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_setfield(L, -2, "window");
}

bool Blend2dRenderer::SetWindowSize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (!window_) {
    window_width_ = width;
    window_height_ = height;
    canvas_width_ = width;
    canvas_height_ = height;
    canvas_scale_x_ = 1.0f;
    canvas_scale_y_ = 1.0f;
    window_to_surface_transform_ = BLMatrix2D::make_identity();
    return true;
  }

  if (!SDL_SetWindowSize(window_, width, height)) {
    return false;
  }

  if (!UpdateWindowMetrics()) {
    return false;
  }

  swapchain_dirty_ = true;
  return true;
}

} // namespace luna::backend::blend2d
