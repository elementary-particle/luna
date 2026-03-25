#ifndef LUNA_RENDERER_H
#define LUNA_RENDERER_H

#include "lua.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <gpu/graphite/vk/VulkanGraphiteTypes.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include <skia/core/SkFont.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkStream.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkTypeface.h>
#include <skia/gpu/graphite/Context.h>

#include "factory.h"

namespace luna {

class Renderer {
private:
  struct PolledEvent {
    std::string type;
    std::optional<std::string> button;
    int x = 0;
    int y = 0;
  };

  struct DeviceCaps {
    uint32_t queue_family_index;
    vk::SurfaceFormatKHR surface_format;
    vk::PresentModeKHR present_mode;
  };

  class LoadImageJob : public AsyncJob {
  public:
    explicit LoadImageJob(skgpu::graphite::Recorder *recorder)
        : recorder_(recorder) {}
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;
    ~LoadImageJob() = default;

  private:
    skgpu::graphite::Recorder *recorder_;
    std::string path_;
    std::unique_ptr<SkStreamAsset> file_;
    sk_sp<SkImage> image_;
  };

  class RegisterFontJob : public AsyncJob {
  public:
    explicit RegisterFontJob(sk_sp<SkFontMgr> font_mgr)
        : font_mgr_(std::move(font_mgr)) {}
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    sk_sp<SkFontMgr> font_mgr_;
    std::string path_;
    std::string family_;
    SkFontStyle style_ = SkFontStyle();
    std::unique_ptr<SkStreamAsset> file_;
  };

  void InitVulkan();
  void CreateSwapchain();
  void InitSkia();
  void FiniSkia();
  void DestroySwapchain();
  bool UpdateWindowMetrics(bool *changed = nullptr);
  bool EnsureGraphicsReady();
  void RecreateSwapchain();
  void PumpSdlEvents();
  void SetFatalError(std::string message);

  static int L_PollSdlEvents(lua_State *L);
  static int L_MakeCanvas(lua_State *L);

  SDL_Window *window_ = nullptr;
  int window_canvas_ref_ = LUA_NOREF;

  vk::Instance vk_instance_;
#if !defined NDEBUG
  vk::DebugUtilsMessengerEXT debug_messenger_;
#endif
  vk::SurfaceKHR surface_;
  vk::PhysicalDevice physical_device_;
  DeviceCaps device_caps_;
  vk::Device device_;
  vk::Queue graphics_queue_;

  vk::SwapchainKHR swapchain_;
  std::vector<vk::Image> swapchain_images_;
  vk::Extent2D surface_extent_;
  std::vector<vk::ImageView> swapchain_image_views_;

  // Synchronization objects
  uint32_t image_count_, image_index_;
  std::deque<vk::Semaphore> acquired_sems_;
  std::vector<vk::Semaphore> signaled_sems_;
  std::vector<vk::Semaphore> rendered_sems_;

  skgpu::graphite::VulkanTextureInfo texture_info_;
  std::unique_ptr<skgpu::graphite::Context> sk_context_;
  std::unique_ptr<skgpu::graphite::Recorder> sk_recorder_;
  sk_sp<SkFontMgr> font_mgr_;

  int window_width_ = 1280;
  int window_height_ = 720;
  int canvas_width_ = 1280;
  int canvas_height_ = 720;
  float canvas_scale_x_ = 1.0f;
  float canvas_scale_y_ = 1.0f;
  bool sdl_ready_ = false;
  bool graphics_ready_ = false;
  bool swapchain_dirty_ = false;
  bool frame_active_ = false;
  bool fatal_error_ = false;
  std::string fatal_error_message_;

  std::vector<PolledEvent> polled_events_;

public:
  Renderer();

  bool Init();
  void Fini();
  bool BeginFrame(lua_State *L);
  bool EndFrame();
  void RegisterBindings(lua_State *L);
  bool SetWindowSize(int width, int height);
  bool HasFatalError() const { return fatal_error_; }
  const std::string &GetFatalError() const { return fatal_error_message_; }

  std::unique_ptr<AsyncJob> MakeLoadImageJob();
  std::unique_ptr<AsyncJob> MakeRegisterFontJob();
};

} // namespace luna

#endif
