#include "renderer.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

#include <fmt/format.h>
#include <skia/codec/SkCodec.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkFontScanner.h>
#include <skia/core/SkRect.h>
#include <skia/gpu/MutableTextureState.h>
#include <skia/gpu/graphite/BackendSemaphore.h>
#include <skia/gpu/graphite/ContextOptions.h>
#include <skia/gpu/graphite/Image.h>
#include <skia/gpu/graphite/Surface.h>
#include <skia/gpu/graphite/vk/VulkanGraphiteContext.h>
#include <skia/gpu/graphite/vk/VulkanGraphiteTypes.h>
#include <skia/gpu/vk/VulkanBackendContext.h>
#include <skia/gpu/vk/VulkanMemoryAllocator.h>
#include <skia/gpu/vk/VulkanMutableTextureState.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include "canvas.h"
#include "font_manager.h"
#include "log.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

sk_sp<skgpu::VulkanMemoryAllocator> MakeVulkanMemoryAllocator(
    VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr,
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr);

namespace luna::backend::skia {

namespace {
SkColorType ToSkColorType(vk::Format format) {
  switch (format) {
  case vk::Format::eB8G8R8A8Unorm:
    return kBGRA_8888_SkColorType;
  case vk::Format::eR8G8B8A8Unorm:
    return kRGBA_8888_SkColorType;
  default:
    return kUnknown_SkColorType;
  }
}

sk_sp<SkColorSpace> ToSkColorSpace(vk::ColorSpaceKHR color_space) {
  if (color_space == vk::ColorSpaceKHR::eSrgbNonlinear) {
    static sk_sp<SkColorSpace> srgb = SkColorSpace::MakeSRGB();
    return srgb;
  }
  return nullptr;
}

SkImageInfo MakeSurfaceImageInfo(
    int width, int height, vk::SurfaceFormatKHR surface_format) {
  return SkImageInfo::Make(width, height, ToSkColorType(surface_format.format),
      kPremul_SkAlphaType, ToSkColorSpace(surface_format.colorSpace));
}

#if defined(TRACY_ENABLE)
static constexpr tracy::SourceLocationData kSkiaGpuFrameSource = {
    "Skia GPU Frame", "SkiaRenderer::EndFrame", __FILE__, __LINE__, 0};

void EmitTracyGpuZoneBegin(TracyVkCtx ctx,
    const tracy::SourceLocationData *source_location, uint16_t query_id) {
  auto *item = tracy::Profiler::QueueSerial();
  tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuZoneBeginSerial);
  tracy::MemWrite(&item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime());
  tracy::MemWrite(
      &item->gpuZoneBegin.srcloc, reinterpret_cast<uint64_t>(source_location));
  tracy::MemWrite(&item->gpuZoneBegin.thread, tracy::GetThreadHandle());
  tracy::MemWrite(&item->gpuZoneBegin.queryId, query_id);
  tracy::MemWrite(&item->gpuZoneBegin.context, ctx->GetId());
  tracy::Profiler::QueueSerialFinish();
}

void EmitTracyGpuZoneEnd(TracyVkCtx ctx, uint16_t query_id) {
  auto *item = tracy::Profiler::QueueSerial();
  tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuZoneEndSerial);
  tracy::MemWrite(&item->gpuZoneEnd.cpuTime, tracy::Profiler::GetTime());
  tracy::MemWrite(&item->gpuZoneEnd.thread, tracy::GetThreadHandle());
  tracy::MemWrite(&item->gpuZoneEnd.queryId, query_id);
  tracy::MemWrite(&item->gpuZoneEnd.context, ctx->GetId());
  tracy::Profiler::QueueSerialFinish();
}
#endif
} // namespace

SkiaRenderer::SkiaRenderer() {}

void SkiaRenderer::SetFatalError(std::string message) {
  fatal_error_ = true;
  fatal_error_message_ = std::move(message);
  log::Error("renderer", "fatal: {}", fatal_error_message_);
}

bool SkiaRenderer::Init() {
  if (!InitSdl(SDL_WINDOW_VULKAN)) {
    return false;
  }

  if (!UpdateWindowMetrics()) {
    if (window_) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    SDL_Quit();
    sdl_ready_ = false;
    return false;
  }

  log::Info("renderer",
      "window created logical={}x{} pixels={}x{} scale={:.2f}", logical_width_,
      logical_height_, pixel_width_, pixel_height_, pixel_viewport_.scale);

  log::Info("renderer", "initialized window state; graphics init deferred");
  return true;
}

#if !defined(NDEBUG)
static VKAPI_ATTR vk::Bool32 VKAPI_CALL DebugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
    vk::DebugUtilsMessageTypeFlagsEXT /*message_type*/,
    const vk::DebugUtilsMessengerCallbackDataEXT *callback_data,
    void * /*user_data*/) {
  log::Level level = log::Level::Warn;
  switch (message_severity) {
  case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
    level = log::Level::Debug;
    break;
  case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
    level = log::Level::Info;
    break;
  case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
    level = log::Level::Warn;
    break;
  case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
    level = log::Level::Error;
    break;
  }
  log::Write(level, "vulkan", callback_data->pMessage);
  return VK_FALSE;
}
#endif

void SkiaRenderer::InitVulkan() {
  log::Info("renderer", "initializing Vulkan");
  VULKAN_HPP_DEFAULT_DISPATCHER.init();
  {
    vk::ApplicationInfo app_info;
    app_info.setPApplicationName("Luna");
    app_info.setApplicationVersion(vk::makeVersion(0, 1, 0));
    app_info.setPEngineName("Luna");
    app_info.setEngineVersion(vk::makeVersion(0, 1, 0));
    app_info.setApiVersion(vk::ApiVersion13);

    std::vector<char const *> layers, extensions;
    {
      uint32_t extension_count;
      char const *const *sdl_extensions =
          SDL_Vulkan_GetInstanceExtensions(&extension_count);
      extensions = {sdl_extensions, sdl_extensions + extension_count};
    }
#if !defined NDEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
    extensions.push_back(vk::EXTDebugUtilsExtensionName);
#endif
    vk::InstanceCreateInfo inst_info{{}, &app_info, layers, extensions};
    vk_instance_ = vk::createInstance(inst_info);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk_instance_);
    log::Debug("renderer", "created Vulkan instance");

#if !defined NDEBUG
    vk::DebugUtilsMessengerCreateInfoEXT create_info{{},
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
            vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
            vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        DebugCallback};

    debug_messenger_ = vk_instance_.createDebugUtilsMessengerEXT(create_info);
#endif
  }

  VkSurfaceKHR surface;
  SDL_Vulkan_CreateSurface(window_, vk_instance_, nullptr, &surface);
  surface_ = surface;
  log::Debug("renderer", "created Vulkan surface");

  {
    auto devices = vk_instance_.enumeratePhysicalDevices();

    auto check_device_caps =
        [surface = surface_](
            vk::PhysicalDevice physical_device) -> std::optional<DeviceCaps> {
      auto surface_formats = physical_device.getSurfaceFormatsKHR(surface);
      auto present_modes = physical_device.getSurfacePresentModesKHR(surface);

      std::optional<vk::SurfaceFormatKHR> surface_format;
      std::optional<vk::PresentModeKHR> present_mode;
      auto properties = physical_device.getProperties();

      for (auto const &format : surface_formats) {
        if (format.format == vk::Format::eB8G8R8A8Unorm &&
            format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
          surface_format = format;
          break;
        }
      }
      if (!surface_format) {
        for (auto const &format : surface_formats) {
          if (format.format == vk::Format::eR8G8B8A8Unorm &&
              format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            surface_format = format;
            break;
          }
        }
      }

      for (auto const &mode : present_modes) {
        if (mode == vk::PresentModeKHR::eFifo) {
          present_mode = mode;
          break;
        }
      }

      std::optional<uint32_t> queue_index;
      auto queue_families = physical_device.getQueueFamilyProperties();
      for (uint32_t i = 0; i < queue_families.size(); i++) {
        if ((queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            physical_device.getSurfaceSupportKHR(i, surface)) {
          queue_index = i;
          break;
        }
      }

      auto queue_family_index =
          queue_index ? fmt::format("{}", *queue_index) : "missing";
      auto surface_format_name =
          surface_format ? vk::to_string(surface_format->format) : "missing";
      auto color_space_name = surface_format
          ? vk::to_string(surface_format->colorSpace)
          : "missing";
      auto present_mode_name =
          present_mode ? vk::to_string(*present_mode) : "missing";
      bool compatible = queue_index.has_value() && surface_format.has_value() &&
          present_mode.has_value();
      log::Debug("renderer",
          "device='{}' queue_family_index={} surface_format={} color_space={} "
          "present_mode={} compatible={}",
          properties.deviceName.data(), queue_family_index, surface_format_name,
          color_space_name, present_mode_name, compatible);

      if (!compatible) {
        return std::nullopt;
      }

      return DeviceCaps{*queue_index, *surface_format, *present_mode};
    };

    for (auto const &device : devices) {
      auto caps = check_device_caps(device);
      if (caps.has_value()) {
        physical_device_ = device;
        device_caps_ = *caps;
      }
    }
  }

  if (!physical_device_) {
    throw std::runtime_error("no compatible Vulkan device found");
  }

  bool calibrated_timestamps = false;
  {
    std::vector<vk::DeviceQueueCreateInfo> queue_info;
    float priority = 1.0f;
    queue_info.push_back(vk::DeviceQueueCreateInfo{
        {}, device_caps_.queue_family_index, 1, &priority});

    std::vector<char const *> device_extensions = {
        vk::KHRSwapchainExtensionName};
    for (auto const &extension :
        physical_device_.enumerateDeviceExtensionProperties()) {
      if (std::strcmp(extension.extensionName,
              VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0) {
        calibrated_timestamps = true;
        break;
      }
    }
    if (calibrated_timestamps) {
      device_extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }

    vk::DeviceCreateInfo device_info{{}, queue_info, {}, device_extensions};
    device_ = physical_device_.createDevice(device_info);

    graphics_queue_ = device_.getQueue(device_caps_.queue_family_index, 0);
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(device_);
  log::Info("renderer", "selected queue_family_index={} present_mode={}",
      device_caps_.queue_family_index,
      vk::to_string(device_caps_.present_mode));

  InitTracyVulkan(calibrated_timestamps);
  CreateSwapchain();
}

void SkiaRenderer::InitTracyVulkan(bool calibrated_timestamps) {
  tracy_vk_ready_ = false;
  tracy_vk_calibrated_ = false;
  tracy_vk_ctx_ = nullptr;

#if defined(TRACY_ENABLE)
  ZoneScopedN("InitTracyVulkan");
  try {
    vk::CommandPoolCreateInfo pool_info{
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        device_caps_.queue_family_index};
    tracy_command_pool_ = device_.createCommandPool(pool_info);

    vk::CommandBufferAllocateInfo alloc_info{
        tracy_command_pool_, vk::CommandBufferLevel::ePrimary, 1};
    tracy_context_command_buffer_ =
        device_.allocateCommandBuffers(alloc_info).front();

    if (calibrated_timestamps) {
      tracy_vk_ctx_ = TracyVkContextCalibrated(vk_instance_, physical_device_,
          device_, graphics_queue_, tracy_context_command_buffer_,
          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
      tracy_vk_calibrated_ = true;
    } else {
      tracy_vk_ctx_ = TracyVkContext(vk_instance_, physical_device_, device_,
          graphics_queue_, tracy_context_command_buffer_,
          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
    }

    TracyVkContextName(static_cast<TracyVkCtx>(tracy_vk_ctx_), "Skia Vulkan",
        static_cast<uint16_t>(std::strlen("Skia Vulkan")));
    tracy_vk_ready_ = true;
    log::Info("renderer", "Tracy Vulkan GPU profiling initialized{}",
        tracy_vk_calibrated_ ? " with calibrated timestamps" : "");
  } catch (const std::exception &e) {
    log::Warn("renderer", "Tracy Vulkan GPU profiling disabled: {}", e.what());
    FiniTracyVulkan();
  }
#else
  (void)calibrated_timestamps;
#endif
}

void SkiaRenderer::FiniTracyVulkan() {
  DestroyTracySwapchainResources();

  if (device_ && tracy_context_command_buffer_) {
    device_.freeCommandBuffers(
        tracy_command_pool_, {tracy_context_command_buffer_});
    tracy_context_command_buffer_ = vk::CommandBuffer{};
  }

  if (tracy_vk_ctx_) {
    TracyVkDestroy(static_cast<TracyVkCtx>(tracy_vk_ctx_));
    tracy_vk_ctx_ = nullptr;
  }

  if (device_ && tracy_command_pool_) {
    device_.destroyCommandPool(tracy_command_pool_);
    tracy_command_pool_ = vk::CommandPool{};
  }

  tracy_vk_ready_ = false;
  tracy_vk_calibrated_ = false;
}

void SkiaRenderer::CreateSwapchain() {
  // Get surface extent
  auto surface_caps = physical_device_.getSurfaceCapabilitiesKHR(surface_);
  if (surface_caps.currentExtent.height !=
      std::numeric_limits<uint32_t>::max()) {
    surface_extent_ = surface_caps.currentExtent;
  } else {
    surface_extent_.width = std::clamp(static_cast<uint32_t>(pixel_width_),
        surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width);
    surface_extent_.height = std::clamp(static_cast<uint32_t>(pixel_height_),
        surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);
  }

  // Create swapchain
  vk::SwapchainCreateInfoKHR swapchain_info;
  swapchain_info.surface = surface_;
  swapchain_info.minImageCount = surface_caps.minImageCount;
  swapchain_info.imageFormat = device_caps_.surface_format.format;
  swapchain_info.imageColorSpace = device_caps_.surface_format.colorSpace;
  swapchain_info.imageExtent = surface_extent_;
  swapchain_info.imageArrayLayers = 1;
  swapchain_info.imageUsage = vk::ImageUsageFlagBits::eTransferSrc |
      vk::ImageUsageFlagBits::eTransferDst |
      vk::ImageUsageFlagBits::eInputAttachment |
      vk::ImageUsageFlagBits::eColorAttachment;
  swapchain_info.imageSharingMode = vk::SharingMode::eExclusive;

  swapchain_info.preTransform = surface_caps.currentTransform;
  swapchain_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
  swapchain_info.presentMode = device_caps_.present_mode;
  swapchain_info.clipped = true;

  swapchain_ = device_.createSwapchainKHR(swapchain_info);
  swapchain_images_ = device_.getSwapchainImagesKHR(swapchain_);
  image_count_ = swapchain_images_.size();
  log::Info("renderer", "created swapchain extent={}x{} images={}",
      surface_extent_.width, surface_extent_.height, image_count_);

  swapchain_image_views_.resize(image_count_);
  // Create swapchain image views
  for (uint32_t i = 0; i < swapchain_image_views_.size(); i++) {
    vk::ImageViewCreateInfo view_info{};
    view_info.image = swapchain_images_[i];
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = device_caps_.surface_format.format;
    view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    swapchain_image_views_[i] = device_.createImageView(view_info);
  }

  // Create synchronization objects
  rendered_sems_.resize(image_count_);
  for (uint32_t i = 0; i < image_count_; i++) {
    acquired_sems_.push_back(device_.createSemaphore({}));
    rendered_sems_[i] = device_.createSemaphore({});
  }
  CreateTracySwapchainResources();

  texture_info_ = {
      static_cast<VkSampleCountFlagBits>(vk::SampleCountFlagBits::e1),
      skgpu::Mipmapped::kNo, 0,
      static_cast<VkFormat>(device_caps_.surface_format.format),
      static_cast<VkImageTiling>(vk::ImageTiling::eOptimal),
      static_cast<VkImageUsageFlags>(vk::ImageUsageFlagBits::eTransferSrc |
          vk::ImageUsageFlagBits::eTransferDst |
          vk::ImageUsageFlagBits::eInputAttachment |
          vk::ImageUsageFlagBits::eColorAttachment),
      static_cast<VkSharingMode>(vk::SharingMode::eExclusive),
      static_cast<VkImageAspectFlags>(vk::ImageAspectFlagBits::eColor), {}};
}

void SkiaRenderer::CreateTracySwapchainResources() {
  if (!tracy_vk_ready_ || !tracy_command_pool_ || image_count_ == 0) {
    return;
  }

  tracy_begin_sems_.resize(image_count_);
  tracy_end_sems_.resize(image_count_);
  for (uint32_t i = 0; i < image_count_; ++i) {
    tracy_begin_sems_[i] = device_.createSemaphore({});
    tracy_end_sems_[i] = device_.createSemaphore({});
  }

  vk::CommandBufferAllocateInfo alloc_info{
      tracy_command_pool_, vk::CommandBufferLevel::ePrimary, image_count_ * 3};
  auto command_buffers = device_.allocateCommandBuffers(alloc_info);
  tracy_collect_command_buffers_.assign(
      command_buffers.begin(), command_buffers.begin() + image_count_);
  tracy_begin_command_buffers_.assign(command_buffers.begin() + image_count_,
      command_buffers.begin() + image_count_ * 2);
  tracy_end_command_buffers_.assign(
      command_buffers.begin() + image_count_ * 2, command_buffers.end());
}

void SkiaRenderer::InitSkia() {
  skgpu::VulkanBackendContext vk_context;
  vk_context.fInstance = vk_instance_;
  vk_context.fPhysicalDevice = physical_device_;
  vk_context.fDevice = device_;
  vk_context.fQueue = graphics_queue_;
  vk_context.fGraphicsQueueIndex = device_caps_.queue_family_index;
  vk_context.fMaxAPIVersion = vk::ApiVersion13;
  vk_context.fGetProc = [](const char *name, VkInstance instance,
                            VkDevice device) {
    PFN_vkVoidFunction p = nullptr;
    if (device != VK_NULL_HANDLE) {
      p = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, name);
    }
    if (p == nullptr) {
      p = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(instance, name);
    }
    return p;
  };
  vk_context.fMemoryAllocator =
      MakeVulkanMemoryAllocator(vk_instance_, physical_device_, device_,
          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);

  skgpu::graphite::ContextOptions options = {};

  sk_context_ =
      skgpu::graphite::ContextFactory::MakeVulkan(vk_context, options);
  if (!sk_context_) {
    throw std::runtime_error("failed to create Skia Graphite Vulkan context");
  }
  sk_recorder_ = sk_context_->makeRecorder();
  if (!sk_recorder_) {
    throw std::runtime_error("failed to create Skia recorder");
  }

  font_mgr_ = MakeRuntimeFontManager();
  log::Info("renderer", "Skia initialized");
}

void SkiaRenderer::FiniSkia() {
  sk_recorder_.reset();
  if (sk_context_) {
    sk_context_->submit(skgpu::graphite::SyncToCpu::kYes);
    if (device_) {
      device_.waitIdle();
    }
    sk_context_.reset();
  }
}

void SkiaRenderer::DestroySwapchain() {
  if (!device_ || !swapchain_) {
    surface_extent_ = vk::Extent2D{};
    return;
  }

  DestroyTracySwapchainResources();

  // Destroy synchronization objects
  for (uint32_t i = 0; i < image_count_; i++) {
    device_.destroySemaphore(rendered_sems_[i]);
    while (!acquired_sems_.empty()) {
      auto sem = acquired_sems_.front();
      acquired_sems_.pop_front();
      device_.destroySemaphore(sem);
    }
  }
  rendered_sems_.clear();

  // Destroy swapchain image views
  for (uint32_t i = 0; i < swapchain_images_.size(); i++) {
    device_.destroyImageView(swapchain_image_views_[i]);
  }
  swapchain_image_views_.clear();

  // Destroy swapchain
  swapchain_images_.clear();
  device_.destroySwapchainKHR(swapchain_);
  swapchain_ = vk::SwapchainKHR{};
  surface_extent_ = vk::Extent2D{};
  image_count_ = 0;
}

void SkiaRenderer::DestroyTracySwapchainResources() {
  if (!device_) {
    tracy_collect_command_buffers_.clear();
    tracy_begin_command_buffers_.clear();
    tracy_end_command_buffers_.clear();
    tracy_begin_sems_.clear();
    tracy_end_sems_.clear();
    return;
  }

  std::vector<vk::CommandBuffer> command_buffers;
  command_buffers.reserve(tracy_collect_command_buffers_.size() +
      tracy_begin_command_buffers_.size() + tracy_end_command_buffers_.size());
  command_buffers.insert(command_buffers.end(),
      tracy_collect_command_buffers_.begin(),
      tracy_collect_command_buffers_.end());
  command_buffers.insert(command_buffers.end(),
      tracy_begin_command_buffers_.begin(), tracy_begin_command_buffers_.end());
  command_buffers.insert(command_buffers.end(),
      tracy_end_command_buffers_.begin(), tracy_end_command_buffers_.end());
  if (!command_buffers.empty() && tracy_command_pool_) {
    device_.freeCommandBuffers(tracy_command_pool_, command_buffers);
  }
  tracy_collect_command_buffers_.clear();
  tracy_begin_command_buffers_.clear();
  tracy_end_command_buffers_.clear();

  for (auto semaphore : tracy_begin_sems_) {
    device_.destroySemaphore(semaphore);
  }
  tracy_begin_sems_.clear();

  for (auto semaphore : tracy_end_sems_) {
    device_.destroySemaphore(semaphore);
  }
  tracy_end_sems_.clear();
}

void SkiaRenderer::Fini() {
  frame_active_ = false;

  if (sk_context_ || sk_recorder_) {
    FiniSkia();
  }
  if (swapchain_) {
    DestroySwapchain();
  }
  if (tracy_vk_ready_ || tracy_vk_ctx_ || tracy_command_pool_) {
    FiniTracyVulkan();
  }

  if (device_) {
    graphics_queue_ = vk::Queue{};
    device_.destroy();
    device_ = vk::Device{};
  }
  device_caps_ = {};
  physical_device_ = vk::PhysicalDevice{};

  if (vk_instance_ && surface_) {
    SDL_Vulkan_DestroySurface(vk_instance_, surface_, nullptr);
    surface_ = vk::SurfaceKHR{};
  }

#if !defined(NDEBUG)
  if (vk_instance_ && debug_messenger_) {
    vk_instance_.destroyDebugUtilsMessengerEXT(debug_messenger_);
    debug_messenger_ = vk::DebugUtilsMessengerEXT{};
  }
#endif

  if (vk_instance_) {
    vk_instance_.destroy();
    vk_instance_ = vk::Instance{};
  }
  graphics_ready_ = false;

  FiniSdl();
}

void SkiaRenderer::ReleaseLua(lua_State *L) {
  if (L != nullptr && window_canvas_ref_ != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, window_canvas_ref_);
  }
  window_canvas_ref_ = LUA_NOREF;
}

bool SkiaRenderer::BeginFrame(lua_State *L) {
  ZoneScopedN("BeginFrame");
  frame_active_ = false;
  PumpSdlEvents();

  if (!EnsureGraphicsReady()) {
    return false;
  }

  if (swapchain_dirty_) {
    RecreateSwapchain();
    if (fatal_error_) {
      return false;
    }
  }

  SkCanvas *sk = sk_recorder_->makeDeferredCanvas(
      MakeSurfaceImageInfo(static_cast<int>(surface_extent_.width),
          static_cast<int>(surface_extent_.height),
          device_caps_.surface_format),
      skgpu::graphite::TextureInfos::MakeVulkan(texture_info_));
  if (!sk) {
    SetFatalError("failed to create deferred canvas");
    return false;
  }
  sk->setMatrix(window_to_surface_matrix_);
  sk->clipRect(SkRect::MakeWH(static_cast<SkScalar>(logical_width_),
                   static_cast<SkScalar>(logical_height_)),
      SkClipOp::kIntersect, true);

  lua_rawgeti(L, LUA_REGISTRYINDEX, window_canvas_ref_);
  if (!lua_isnil(L, -1)) {
    Canvas *canvas = static_cast<Canvas *>(lua_touserdata(L, -1));
    canvas->set_font_manager(font_mgr_);
    canvas->set_sk(sk);
    canvas->set_window_to_surface_matrix(window_to_surface_matrix_);
  }
  lua_pop(L, 1);
  frame_active_ = true;
  return true;
}

bool SkiaRenderer::SubmitTracyCollect(vk::CommandBuffer command_buffer) {
#if !defined(TRACY_ENABLE)
  (void)command_buffer;
  return true;
#else
  if (!tracy_vk_ready_ || !tracy_vk_ctx_) {
    return true;
  }

  graphics_queue_.waitIdle();

  auto ctx = static_cast<TracyVkCtx>(tracy_vk_ctx_);
  command_buffer.reset();
  vk::CommandBufferBeginInfo begin_info{
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  command_buffer.begin(begin_info);
  TracyVkCollect(ctx, command_buffer);
  command_buffer.end();

  vk::SubmitInfo submit_info;
  submit_info.setCommandBuffers(command_buffer);
  graphics_queue_.submit(submit_info);
  return true;
#endif
}

bool SkiaRenderer::SubmitTracyTimestamp(vk::CommandBuffer command_buffer,
    vk::Semaphore wait_semaphore, vk::Semaphore signal_semaphore,
    uint16_t query_id) {
#if !defined(TRACY_ENABLE)
  (void)command_buffer;
  (void)wait_semaphore;
  (void)signal_semaphore;
  (void)query_id;
  return true;
#else
  if (!tracy_vk_ready_ || !tracy_vk_ctx_) {
    return true;
  }

  auto ctx = static_cast<TracyVkCtx>(tracy_vk_ctx_);
  command_buffer.reset();
  vk::CommandBufferBeginInfo begin_info{
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
  command_buffer.begin(begin_info);
  command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
      static_cast<vk::QueryPool>(ctx->GetQueryPool()), query_id);
  command_buffer.end();

  vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTopOfPipe;
  vk::SubmitInfo submit_info;
  submit_info.setWaitSemaphores(wait_semaphore);
  submit_info.setWaitDstStageMask(wait_stage);
  submit_info.setCommandBuffers(command_buffer);
  submit_info.setSignalSemaphores(signal_semaphore);
  graphics_queue_.submit(submit_info);
  return true;
#endif
}

bool SkiaRenderer::EndFrame() {
  ZoneScopedN("EndFrame");
  if (!frame_active_) {
    return !fatal_error_;
  }

  auto recording = sk_recorder_->snap();
  if (!recording) {
    SetFatalError("failed to snap Skia recording");
    frame_active_ = false;
    return false;
  }

  vk::Semaphore acquired;
  if (acquired_sems_.empty()) {
    sk_context_->submit(skgpu::graphite::SyncToCpu::kYes);
  }
  acquired = acquired_sems_.front();
  acquired_sems_.pop_front();
  signaled_sems_.push_back(acquired);

  auto next_index = device_.acquireNextImageKHR(
      swapchain_, std::numeric_limits<uint64_t>::max(), acquired);
  if (next_index.result == vk::Result::eTimeout ||
      next_index.result == vk::Result::eNotReady) {
    log::Warn("renderer", "acquireNextImageKHR returned {}",
        vk::to_string(next_index.result));
    frame_active_ = false;
    return true;
  }
  if (next_index.result != vk::Result::eSuccess &&
      next_index.result != vk::Result::eSuboptimalKHR) {
    if (next_index.result == vk::Result::eErrorOutOfDateKHR) {
      swapchain_dirty_ = true;
    }
    SetFatalError(fmt::format(
        "acquireNextImageKHR failed: {}", vk::to_string(next_index.result)));
    frame_active_ = false;
    return false;
  }
  if (next_index.result == vk::Result::eSuboptimalKHR) {
    swapchain_dirty_ = true;
  }
  image_index_ = next_index.value;

  const bool profile_gpu_frame = tracy_vk_ready_ &&
      image_index_ < tracy_begin_command_buffers_.size() &&
      image_index_ < tracy_end_command_buffers_.size() &&
      image_index_ < tracy_collect_command_buffers_.size() &&
      image_index_ < tracy_begin_sems_.size() &&
      image_index_ < tracy_end_sems_.size();

  uint16_t tracy_begin_query = 0;
  uint16_t tracy_end_query = 0;
  vk::Image next_image = swapchain_images_[image_index_];
  SkSurfaceProps props{};
  sk_sp<SkSurface> surface = SkSurfaces::WrapBackendTexture(sk_recorder_.get(),
      skgpu::graphite::BackendTextures::MakeVulkan(
          {static_cast<int32_t>(surface_extent_.width),
              static_cast<int32_t>(surface_extent_.height)},
          texture_info_,
          static_cast<VkImageLayout>(vk::ImageLayout::eUndefined),
          vk::QueueFamilyIgnored, next_image, {}),
      nullptr, &props);

  if (!surface) {
    SetFatalError(fmt::format(
        "failed to wrap Skia backend texture image_index={} extent={}x{}",
        image_index_, surface_extent_.width, surface_extent_.height));
    frame_active_ = false;
    return false;
  }

  if (profile_gpu_frame) {
#if defined(TRACY_ENABLE)
    auto ctx = static_cast<TracyVkCtx>(tracy_vk_ctx_);
    SubmitTracyCollect(tracy_collect_command_buffers_[image_index_]);

    tracy_begin_query = static_cast<uint16_t>(ctx->NextQueryId());
    EmitTracyGpuZoneBegin(ctx, &kSkiaGpuFrameSource, tracy_begin_query);

    SubmitTracyTimestamp(tracy_begin_command_buffers_[image_index_], acquired,
        tracy_begin_sems_[image_index_], tracy_begin_query);
#endif
  }

  skgpu::graphite::BackendSemaphore wait =
      skgpu::graphite::BackendSemaphores::MakeVulkan(
          profile_gpu_frame ? tracy_begin_sems_[image_index_] : acquired);
  skgpu::graphite::BackendSemaphore signal =
      skgpu::graphite::BackendSemaphores::MakeVulkan(
          rendered_sems_[image_index_]);

  skgpu::graphite::InsertRecordingInfo recording_info = {
      recording.get(),
      surface.get(),
  };

  skgpu::MutableTextureState present_state =
      skgpu::MutableTextureStates::MakeVulkan(
          static_cast<VkImageLayout>(vk::ImageLayout::ePresentSrcKHR),
          device_caps_.queue_family_index);
  recording_info.fTargetTextureState = &present_state;
  recording_info.fNumWaitSemaphores = 1;
  recording_info.fWaitSemaphores = &wait;
  recording_info.fNumSignalSemaphores = 1;
  recording_info.fSignalSemaphores = &signal;
  recording_info.fFinishedContext =
      new std::function([this, signaled_sems = std::move(signaled_sems_)]() {
        for (auto &sem : signaled_sems) {
          acquired_sems_.push_back(sem);
        }
      });
  recording_info.fFinishedProc = [](skgpu::graphite::GpuFinishedContext ctx,
                                     skgpu::CallbackResult) {
    auto *callback = reinterpret_cast<std::function<void()> *>(ctx);
    (*callback)();
    delete callback;
  };
  // Allow 1 frame in flight.
  {
    ZoneScopedN("WaitQueueIdle");
    sk_context_->submit(skgpu::graphite::SyncToCpu::kYes);
  }
  {
    ZoneScopedN("SubmitRender");
    sk_context_->insertRecording(recording_info);
    sk_context_->submit();
  }

  vk::Semaphore present_wait = rendered_sems_[image_index_];
  if (profile_gpu_frame) {
#if defined(TRACY_ENABLE)
    auto ctx = static_cast<TracyVkCtx>(tracy_vk_ctx_);
    tracy_end_query = static_cast<uint16_t>(ctx->NextQueryId());
    EmitTracyGpuZoneEnd(ctx, tracy_end_query);
    SubmitTracyTimestamp(tracy_end_command_buffers_[image_index_],
        rendered_sems_[image_index_], tracy_end_sems_[image_index_],
        tracy_end_query);
    present_wait = tracy_end_sems_[image_index_];
#endif
  }

  vk::PresentInfoKHR present_info{{present_wait}, {swapchain_}, {image_index_}};
  auto result = graphics_queue_.presentKHR(present_info);
  if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
    if (result == vk::Result::eErrorOutOfDateKHR) {
      swapchain_dirty_ = true;
    }
    SetFatalError(fmt::format("presentKHR failed for image_index={}: {}",
        image_index_, vk::to_string(result)));
    frame_active_ = false;
    return false;
  }
  if (result == vk::Result::eSuboptimalKHR) {
    swapchain_dirty_ = true;
  }
  frame_active_ = false;
  // log::Debug("renderer", "presented frame image_index={}", image_index_);
  return true;
}

void SkiaRenderer::UpdateWindowTransform() {
  window_to_surface_matrix_.setAll(static_cast<SkScalar>(pixel_viewport_.scale),
      0.0f, static_cast<SkScalar>(pixel_viewport_.x), 0.0f,
      static_cast<SkScalar>(pixel_viewport_.scale),
      static_cast<SkScalar>(pixel_viewport_.y), 0.0f, 0.0f, 1.0f);
}

bool SkiaRenderer::EnsureGraphicsReady() {
  if (fatal_error_) {
    return false;
  }
  if (graphics_ready_) {
    return true;
  }

  try {
    InitVulkan();
    InitSkia();
  } catch (const std::exception &e) {
    SetFatalError(fmt::format("graphics initialization failed: {}", e.what()));
    return false;
  }

  graphics_ready_ = true;
  swapchain_dirty_ = false;
  return true;
}

void SkiaRenderer::RecreateSwapchain() {
  if (!UpdateWindowMetrics()) {
    return;
  }

  if (device_) {
    device_.waitIdle();
  }
  DestroySwapchain();
  try {
    CreateSwapchain();
  } catch (const std::exception &e) {
    SetFatalError(fmt::format("swapchain recreation failed: {}", e.what()));
    return;
  }
  swapchain_dirty_ = false;
  log::Info("renderer",
      "recreated swapchain logical={}x{} pixels={}x{} scale={:.2f}",
      logical_width_, logical_height_, pixel_width_, pixel_height_,
      pixel_viewport_.scale);
}

std::unique_ptr<AsyncJob> SkiaRenderer::MakeLoadImageJob() {
  return std::make_unique<LoadImageJob>(sk_recorder_.get());
}

void SkiaRenderer::LoadImageJob::Invoke(lua_State *L) {
  input_ = AssetInput::Check(L, 2);
  path_ = input_.path;
}

void SkiaRenderer::LoadImageJob::Run() {
  status_ = input_.Map(&mapping_);
  if (!status_)
    return;
  auto data = SkData::MakeWithProc(
      mapping_.data(), mapping_.size(),
      [](const void *, void *owner) {
        delete static_cast<file::MappedFile *>(owner);
      },
      new file::MappedFile(mapping_));
  file_ = std::make_unique<SkMemoryStream>(std::move(data));
  ZoneScopedN("LoadImage");
  std::unique_ptr<SkCodec> codec = SkCodec::MakeFromStream(std::move(file_));
  if (!codec) {
    DecodeError(path_, fmt::format("unrecognized image format: {}", path_));
    return;
  }
  SkCodec::Result result;
  std::tie(image_, result) = codec->getImage();
  if (result != SkCodec::kSuccess) {
    DecodeError(path_,
        fmt::format(
            "failed to decode image: {}, {}", path_, fmt::underlying(result)));
    return;
  }
}

int SkiaRenderer::LoadImageJob::Finish(lua_State *L) {
  ZoneScopedN("FinishLoadImage");
  image_ = SkImages::TextureFromImage(recorder_, image_);
  if (!image_) {
    Fail("failed to upload decoded image");
    return 0;
  }
  auto *image = lua::New<Image>(L, std::move(image_));

  lua_pushnumber(L, image->sk->width());
  lua_pushnumber(L, image->sk->height());

  return 3;
}

std::unique_ptr<AsyncJob> SkiaRenderer::MakeLoadFontfaceJob() {
  return std::make_unique<LoadFontfaceJob>(font_mgr_);
}

void SkiaRenderer::LoadFontfaceJob::Invoke(lua_State *L) {
  input_ = AssetInput::Check(L, 2);
  path_ = input_.path;
}

void SkiaRenderer::LoadFontfaceJob::Run() {
  status_ = input_.Map(&mapping_);
  if (!status_)
    return;
  ZoneScopedN("LoadFontface");
  if (!font_mgr_ || !RegisterRuntimeFont(font_mgr_, std::move(mapping_))) {
    DecodeError(path_, fmt::format("failed to register font '{}'", path_));
  }
}

int SkiaRenderer::LoadFontfaceJob::Finish(lua_State *L) {
  lua_pushboolean(L, true);
  return 1;
}

int SkiaRenderer::L_MakeCanvas(lua_State *L) {
  SkiaRenderer *r =
      static_cast<SkiaRenderer *>(lua_touserdata(L, lua_upvalueindex(1)));
  int width = static_cast<int>(luaL_checkinteger(L, 1));
  int height = static_cast<int>(luaL_checkinteger(L, 2));

  if (width <= 0 || height <= 0) {
    return luaL_error(L, "make_canvas: width and height must be positive");
  }

  sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(r->sk_recorder_.get(),
      MakeSurfaceImageInfo(width, height, r->device_caps_.surface_format));
  if (!surface) {
    return luaL_error(L, "make_canvas: failed to create render target");
  }

  Canvas *canvas = lua::New<Canvas>(L);
  canvas->set_font_manager(r->font_mgr_);
  canvas->set_surface(surface);
  return 1;
}

void SkiaRenderer::BindLua(lua_State *L) {
  if (lua::NewType<Image>(L)) {
    lua_pushcfunction(L, [](lua_State *L) {
      Image *image = lua::Check<Image>(L, 1);
      image->sk.reset();
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
  lua::Check<Canvas>(L, -1)->set_font_manager(font_mgr_);
  lua_pushvalue(L, -1);
  window_canvas_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_setfield(L, -2, "window");
}

} // namespace luna::backend::skia
