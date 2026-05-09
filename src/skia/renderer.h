#ifndef LUNA_SKIA_RENDERER_H
#define LUNA_SKIA_RENDERER_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <memory>
#include <string>
#include <vector>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include <skia/core/SkFont.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkMatrix.h>
#include <skia/core/SkStream.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkTypeface.h>
#include <skia/gpu/graphite/Context.h>
#include <skia/gpu/graphite/vk/VulkanGraphiteTypes.h>

#include "asset_vfs.h"
#include "factory.h"
#include "renderer_interface.h"

struct lua_State;

namespace luna::backend::skia {

class SkiaRenderer final : public Renderer {
private:
  struct DeviceCaps {
    uint32_t queue_family_index;
    vk::SurfaceFormatKHR surface_format;
    vk::PresentModeKHR present_mode;
  };

  class LoadImageJob : public AsyncJob {
  public:
    LoadImageJob(skgpu::graphite::Recorder *recorder, asset::Vfs *vfs)
        : recorder_(recorder), vfs_(vfs) {}
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;
    ~LoadImageJob() = default;

  private:
    skgpu::graphite::Recorder *recorder_;
    asset::Vfs *vfs_ = nullptr;
    std::string path_;
    asset::MappedAsset mapping_;
    std::unique_ptr<SkStreamAsset> file_;
    sk_sp<SkImage> image_;
  };

  class LoadFontfaceJob : public AsyncJob {
  public:
    LoadFontfaceJob(sk_sp<SkFontMgr> font_mgr, asset::Vfs *vfs)
        : font_mgr_(std::move(font_mgr)), vfs_(vfs) {}
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    sk_sp<SkFontMgr> font_mgr_;
    asset::Vfs *vfs_ = nullptr;
    std::string path_;
    asset::MappedAsset mapping_;
  };

  void InitVulkan();
  void InitTracyVulkan(bool calibrated_timestamps);
  void FiniTracyVulkan();
  void CreateSwapchain();
  void CreateTracySwapchainResources();
  void DestroyTracySwapchainResources();
  void InitSkia();
  void FiniSkia();
  void DestroySwapchain();
  bool UpdateWindowMetrics(bool *changed = nullptr);
  bool EnsureGraphicsReady();
  void RecreateSwapchain();
  void SetFatalError(std::string message);
  bool SubmitTracyCollect(vk::CommandBuffer command_buffer);
  bool SubmitTracyTimestamp(
      vk::CommandBuffer command_buffer, vk::Semaphore wait_semaphore,
      vk::Semaphore signal_semaphore, uint16_t query_id);

  static int L_MakeCanvas(lua_State *L);

  int window_canvas_ref_ = LUA_NOREF;

  vk::Instance vk_instance_;
#if !defined NDEBUG
  vk::DebugUtilsMessengerEXT debug_messenger_;
#endif
  vk::SurfaceKHR surface_;
  vk::PhysicalDevice physical_device_;
  DeviceCaps device_caps_ = {0,
      {vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear},
      vk::PresentModeKHR::eFifo};
  vk::Device device_;
  vk::Queue graphics_queue_;

  void *tracy_vk_ctx_ = nullptr;
  vk::CommandPool tracy_command_pool_;
  vk::CommandBuffer tracy_context_command_buffer_;
  std::vector<vk::CommandBuffer> tracy_collect_command_buffers_;
  std::vector<vk::CommandBuffer> tracy_begin_command_buffers_;
  std::vector<vk::CommandBuffer> tracy_end_command_buffers_;
  std::vector<vk::Semaphore> tracy_begin_sems_;
  std::vector<vk::Semaphore> tracy_end_sems_;
  bool tracy_vk_ready_ = false;
  bool tracy_vk_calibrated_ = false;

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

  int canvas_width_ = 1280;
  int canvas_height_ = 720;
  float canvas_scale_x_ = 1.0f;
  float canvas_scale_y_ = 1.0f;
  SkMatrix window_to_surface_matrix_ = SkMatrix::I();
  bool graphics_ready_ = false;
  bool frame_active_ = false;
  bool fatal_error_ = false;
  std::string fatal_error_message_;

public:
  SkiaRenderer();
  ~SkiaRenderer() override = default;

  bool Init() override;
  void ReleaseLua(lua_State *L) override;
  void Fini() override;
  bool BeginFrame(lua_State *L) override;
  bool EndFrame() override;
  void BindLua(lua_State *L) override;
  bool SetWindowSize(int width, int height) override;
  bool HasFatalError() const override { return fatal_error_; }
  const std::string &GetFatalError() const override {
    return fatal_error_message_;
  }

  std::unique_ptr<AsyncJob> MakeLoadImageJob(asset::Vfs *vfs) override;
  std::unique_ptr<AsyncJob> MakeLoadFontfaceJob(asset::Vfs *vfs) override;
};

} // namespace luna::backend::skia

#endif
