#include "renderer.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000
#include <vk_mem_alloc.h>

#include <fmt/format.h>
#include <skia/codec/SkCodec.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkFontScanner.h>
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

#if defined(SK_FONTMGR_FONTCONFIG_AVAILABLE)
#include "skia/ports/SkFontMgr_fontconfig.h"
#include "skia/ports/SkFontScanner_FreeType.h"
#endif

#include <algorithm>
#include <optional>
#include <stdexcept>

#include "canvas.h"
#include "log.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

sk_sp<skgpu::VulkanMemoryAllocator>
MakeVulkanMemoryAllocator(VkInstance instance, VkPhysicalDevice physical_device,
                          VkDevice device,
                          PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr,
                          PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr);

namespace luna {

static constexpr const char *RENDERER_PTR_KEY = "luna.renderer_ptr";

Renderer::Renderer() {}

void Renderer::SetFatalError(std::string message) {
  fatal_error_ = true;
  fatal_error_message_ = std::move(message);
  log::Error("renderer", "fatal: {}", fatal_error_message_);
}

bool Renderer::Init() {
  log::Info("renderer", "initializing SDL video/audio/events");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
    log::Error("renderer", "SDL_Init failed: {}", SDL_GetError());
    return false;
  }
  sdl_ready_ = true;

  SDL_WindowFlags window_flags =
      SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  window_ =
      SDL_CreateWindow("Luna", window_width_, window_height_, window_flags);
  if (!window_) {
    log::Error("renderer", "SDL_CreateWindow failed: {}", SDL_GetError());
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

  log::Info("renderer", "window created size={}x{} canvas={}x{} scale={}x{}",
            window_width_,
            window_height_,
            canvas_width_,
            canvas_height_,
            canvas_scale_x_,
            canvas_scale_y_);

  log::Info("renderer", "initialized window state; graphics init deferred");
  return true;
}

#if !defined(NDEBUG)
static VKAPI_ATTR vk::Bool32 VKAPI_CALL
DebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT /*message_severity*/,
              vk::DebugUtilsMessageTypeFlagsEXT /*message_type*/,
              const vk::DebugUtilsMessengerCallbackDataEXT *callback_data,
              void * /*user_data*/) {
  log::Warn("vulkan", "{}", callback_data->pMessage);
  return VK_FALSE;
}
#endif

void Renderer::InitVulkan() {
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
    vk::DebugUtilsMessengerCreateInfoEXT create_info{
        {},
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

      for (auto const &format : surface_formats) {
        if (format.format == vk::Format::eR8G8B8A8Unorm &&
            format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
          surface_format = format;
        }
      }

      for (auto const &mode : present_modes) {
        if (mode == vk::PresentModeKHR::eMailbox) {
          present_mode = mode;
        } else if (mode == vk::PresentModeKHR::eFifo &&
                   present_mode != vk::PresentModeKHR::eMailbox) {
          present_mode = mode;
        }
      }

      if (!(surface_format && present_mode)) {
        return std::nullopt;
      }

      std::optional<uint32_t> queue_index;
      auto queue_families = physical_device.getQueueFamilyProperties();
      for (uint32_t i = 0; i < queue_families.size(); i++) {
        if ((queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            physical_device.getSurfaceSupportKHR(i, surface)) {
          queue_index = i;
        }
      }

      if (!queue_index.has_value()) {
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

  {
    std::vector<vk::DeviceQueueCreateInfo> queue_info;
    float priority = 1.0f;
    queue_info.push_back(vk::DeviceQueueCreateInfo{
        {}, device_caps_.queue_family_index, 1, &priority});

    vk::DeviceCreateInfo device_info{
        {}, queue_info, {}, {vk::KHRSwapchainExtensionName}};
    device_ = physical_device_.createDevice(device_info);

    graphics_queue_ = device_.getQueue(device_caps_.queue_family_index, 0);
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init(device_);
  log::Info("renderer", "selected queue_family_index={} present_mode={}",
            device_caps_.queue_family_index,
            vk::to_string(device_caps_.present_mode));

  CreateSwapchain();
}

void Renderer::CreateSwapchain() {
  // Get surface extent
  auto surface_caps = physical_device_.getSurfaceCapabilitiesKHR(surface_);
  if (surface_caps.currentExtent.height !=
      std::numeric_limits<uint32_t>::max()) {
    surface_extent_ = surface_caps.currentExtent;
  } else {
    surface_extent_.width = std::clamp(static_cast<uint32_t>(canvas_width_),
                                       surface_caps.minImageExtent.width,
                                       surface_caps.maxImageExtent.width);
    surface_extent_.height = std::clamp(static_cast<uint32_t>(canvas_height_),
                                        surface_caps.minImageExtent.height,
                                        surface_caps.maxImageExtent.height);
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

  texture_info_ = {
      static_cast<VkSampleCountFlagBits>(vk::SampleCountFlagBits::e1),
      skgpu::Mipmapped::kNo,
      0,
      static_cast<VkFormat>(vk::Format::eR8G8B8A8Unorm),
      static_cast<VkImageTiling>(vk::ImageTiling::eOptimal),
      static_cast<VkImageUsageFlags>(vk::ImageUsageFlagBits::eTransferSrc |
                                     vk::ImageUsageFlagBits::eTransferDst |
                                     vk::ImageUsageFlagBits::eInputAttachment |
                                     vk::ImageUsageFlagBits::eColorAttachment),
      static_cast<VkSharingMode>(vk::SharingMode::eExclusive),
      static_cast<VkImageAspectFlags>(vk::ImageAspectFlagBits::eColor),
      {}};
}

void Renderer::InitSkia() {
  skgpu::VulkanBackendContext vk_context;
  vk_context.fInstance = vk_instance_;
  vk_context.fPhysicalDevice = physical_device_;
  vk_context.fDevice = device_;
  vk_context.fQueue = graphics_queue_;
  vk_context.fGraphicsQueueIndex = device_caps_.queue_family_index;
  vk_context.fMaxAPIVersion = vk::ApiVersion13;
  vk_context.fGetProc = [](const char *name,
                           VkInstance instance,
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
  vk_context.fMemoryAllocator = MakeVulkanMemoryAllocator(
      vk_instance_,
      physical_device_,
      device_,
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

#if defined(SK_FONTMGR_FONTCONFIG_AVAILABLE)
  font_mgr_ = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
  log::Info("renderer", "Skia initialized");
}

void Renderer::FiniSkia() {
  sk_recorder_.reset();
  if (sk_context_) {
    sk_context_->submit(skgpu::graphite::SyncToCpu::kYes);
    if (device_) {
      device_.waitIdle();
    }
    sk_context_.reset();
  }
}

void Renderer::DestroySwapchain() {
  if (!device_ || !swapchain_) {
    surface_extent_ = vk::Extent2D{};
    return;
  }

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

void Renderer::Fini() {
  frame_active_ = false;

  if (sk_context_ || sk_recorder_) {
    FiniSkia();
  }
  if (swapchain_) {
    DestroySwapchain();
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

  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  if (sdl_ready_) {
    SDL_Quit();
    sdl_ready_ = false;
  }
}

bool Renderer::BeginFrame(lua_State *L) {
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
      SkImageInfo::Make(surface_extent_.width,
                        surface_extent_.height,
                        kRGBA_8888_SkColorType,
                        kPremul_SkAlphaType),
      skgpu::graphite::TextureInfos::MakeVulkan(texture_info_));
  if (!sk) {
    SetFatalError("failed to create deferred canvas");
    return false;
  }
  sk->scale(SkFloatToScalar(canvas_scale_x_), SkFloatToScalar(canvas_scale_y_));

  lua_rawgeti(L, LUA_REGISTRYINDEX, window_canvas_ref_);
  if (!lua_isnil(L, -1)) {
    LCanvas *canvas = static_cast<LCanvas *>(lua_touserdata(L, -1));
    canvas->set_font_manager(font_mgr_);
    canvas->set_sk(sk);
  }
  lua_pop(L, 1);
  frame_active_ = true;
  return true;
}

bool Renderer::EndFrame() {
  if (!frame_active_) {
    return !fatal_error_;
  }

  std::unique_ptr<skgpu::graphite::Recording> recording = sk_recorder_->snap();
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
    SetFatalError(
        fmt::format("acquireNextImageKHR failed: {}",
                    vk::to_string(next_index.result)));
    frame_active_ = false;
    return false;
  }
  if (next_index.result == vk::Result::eSuboptimalKHR) {
    swapchain_dirty_ = true;
  }
  image_index_ = next_index.value;

  vk::Image next_image = swapchain_images_[image_index_];
  SkSurfaceProps props{};
  sk_sp<SkSurface> surface = SkSurfaces::WrapBackendTexture(
      sk_recorder_.get(),
      skgpu::graphite::BackendTextures::MakeVulkan(
          {static_cast<int32_t>(surface_extent_.width),
           static_cast<int32_t>(surface_extent_.height)},
          texture_info_,
          static_cast<VkImageLayout>(vk::ImageLayout::eUndefined),
          vk::QueueFamilyIgnored,
          next_image,
          {}),
      nullptr,
      &props);

  if (!surface) {
    SetFatalError(fmt::format(
        "failed to wrap Skia backend texture image_index={} extent={}x{}",
        image_index_,
        surface_extent_.width,
        surface_extent_.height));
    frame_active_ = false;
    return false;
  }

  skgpu::graphite::BackendSemaphore wait =
      skgpu::graphite::BackendSemaphores::MakeVulkan(acquired);
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
  sk_context_->submit(skgpu::graphite::SyncToCpu::kYes);
  sk_context_->insertRecording(recording_info);
  sk_context_->submit();

  vk::PresentInfoKHR present_info{
      {rendered_sems_[image_index_]}, {swapchain_}, {image_index_}};
  auto result = graphics_queue_.presentKHR(present_info);
  if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
    if (result == vk::Result::eErrorOutOfDateKHR) {
      swapchain_dirty_ = true;
    }
    SetFatalError(fmt::format("presentKHR failed for image_index={}: {}",
                              image_index_,
                              vk::to_string(result)));
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

void Renderer::PumpSdlEvents() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
      polled_events_.push_back(PolledEvent{"quit", std::nullopt, 0, 0});
      break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      swapchain_dirty_ = true;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      polled_events_.push_back(PolledEvent{
          "mouse_move", std::nullopt, (int)e.motion.x, (int)e.motion.y});
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      std::optional<std::string> button;
      if (e.button.button == SDL_BUTTON_LEFT) {
        button = "left";
      } else if (e.button.button == SDL_BUTTON_RIGHT) {
        button = "right";
      } else if (e.button.button == SDL_BUTTON_MIDDLE) {
        button = "middle";
      }
      polled_events_.push_back(PolledEvent{
          e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "mouse_down" : "mouse_up",
          button,
          (int)e.button.x,
          (int)e.button.y});
      break;
    }
    default:
      break;
    }
  }
}

bool Renderer::UpdateWindowMetrics(bool *changed) {
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
    log::Error("renderer", "SDL_GetWindowSizeInPixels failed: {}",
               SDL_GetError());
    return false;
  }

  if (window_width <= 0 || window_height <= 0 || canvas_width <= 0 ||
      canvas_height <= 0) {
    log::Warn("renderer",
              "ignoring non-positive window/canvas size window={}x{} canvas={}x{}",
              window_width,
              window_height,
              canvas_width,
              canvas_height);
    return false;
  }

  const bool metrics_changed = window_width_ != window_width ||
                               window_height_ != window_height ||
                               canvas_width_ != canvas_width ||
                               canvas_height_ != canvas_height;

  window_width_ = window_width;
  window_height_ = window_height;
  canvas_width_ = canvas_width;
  canvas_height_ = canvas_height;
  canvas_scale_x_ =
      static_cast<float>(canvas_width_) / static_cast<float>(window_width_);
  canvas_scale_y_ =
      static_cast<float>(canvas_height_) / static_cast<float>(window_height_);

  if (changed) {
    *changed = metrics_changed;
  }
  return true;
}

bool Renderer::EnsureGraphicsReady() {
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

void Renderer::RecreateSwapchain() {
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
  log::Info("renderer", "recreated swapchain window={}x{} canvas={}x{} scale={:.2f}x{:.2f}",
            window_width_,
            window_height_,
            canvas_width_,
            canvas_height_,
            canvas_scale_x_,
            canvas_scale_y_);
}

std::unique_ptr<AsyncJob> Renderer::MakeLoadImageJob() {
  return std::make_unique<LoadImageJob>(sk_recorder_.get());
}

void Renderer::LoadImageJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!path || !*path) {
    luaL_error(L, "load_image: path is empty");
  }
  path_ = path;
  file_ = SkStreamAsset::MakeFromFile(path);
  if (!file_) {
    luaL_error(L, "load_image: failed to open file: %s", path);
  }
}

void Renderer::LoadImageJob::Run() {
  std::unique_ptr<SkCodec> codec = SkCodec::MakeFromStream(std::move(file_));
  if (!codec) {
    error_ = fmt::format("unrecognized image format: {}", path_);
    return;
  }
  image_ = SkCodecs::DeferredImage(std::move(codec));
}

int Renderer::LoadImageJob::Finish(lua_State *L) {
  image_ = SkImages::TextureFromImage(recorder_, image_);
  auto *image = lua::New<LImage>(L, std::move(image_));

  lua_pushnumber(L, image->sk->width());
  lua_pushnumber(L, image->sk->height());

  return 3;
}

std::unique_ptr<AsyncJob> Renderer::MakeLoadTypefaceJob() {
  return std::make_unique<LoadTypefaceJob>(font_mgr_);
}

void Renderer::LoadTypefaceJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  if (!path || !*path)
    luaL_error(L, "load_font: path is empty");

  path_ = path;
  file_ = SkStreamAsset::MakeFromFile(path);
  if (!file_) {
    luaL_error(L, "load_font: failed to open file: %s", path);
  }
}

void Renderer::LoadTypefaceJob::Run() {
  if (font_mgr_) {
    typeface_ = font_mgr_->makeFromFile(path_.c_str());
  }
  if (!typeface_) {
    error_ = fmt::format("unrecognized font format: {}", path_);
  }
}

int Renderer::LoadTypefaceJob::Finish(lua_State *L) {
  lua::New<LTypeface>(L, std::move(typeface_));
  return 1;
}

int Renderer::L_PollSdlEvents(lua_State *L) {
  Renderer *r = static_cast<Renderer *>(lua_touserdata(L, lua_upvalueindex(1)));
  lua_newtable(L);
  int out_i = 1;
  for (const PolledEvent &ev : r->polled_events_) {
    lua_newtable(L);
    lua_pushstring(L, ev.type.c_str());
    lua_setfield(L, -2, "type");
    if (ev.button.has_value()) {
      lua_pushstring(L, ev.button->c_str());
      lua_setfield(L, -2, "button");
    }
    lua_pushinteger(L, ev.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, ev.y);
    lua_setfield(L, -2, "y");
    lua_rawseti(L, -2, out_i);
    out_i++;
  }
  r->polled_events_.clear();
  return 1;
}

int Renderer::L_MakeCanvas(lua_State *L) {
  Renderer *r = static_cast<Renderer *>(lua_touserdata(L, lua_upvalueindex(1)));
  int width = static_cast<int>(luaL_checkinteger(L, 1));
  int height = static_cast<int>(luaL_checkinteger(L, 2));

  if (width <= 0 || height <= 0) {
    return luaL_error(L, "make_canvas: width and height must be positive");
  }

  sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(
      r->sk_recorder_.get(),
      SkImageInfo::Make(
          width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
  if (!surface) {
    return luaL_error(L, "make_canvas: failed to create render target");
  }

  LCanvas *canvas = lua::New<LCanvas>(L);
  canvas->set_font_manager(r->font_mgr_);
  canvas->set_surface(surface);
  return 1;
}

void Renderer::RegisterBindings(lua_State *L) {
  lua::NewType<LImage>(L);
  lua_pop(L, 1);
  lua::NewType<LTypeface>(L);
  lua_pop(L, 1);
  LCanvas::RegisterBindings(L);

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_PollSdlEvents, 1);
  lua_setfield(L, -2, "poll_events");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_MakeCanvas, 1);
  lua_setfield(L, -2, "make_canvas");

  lua::New<LCanvas>(L);
  lua::Check<LCanvas>(L, -1)->set_font_manager(font_mgr_);
  lua_pushvalue(L, -1);
  window_canvas_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_setfield(L, -2, "window");
}

bool Renderer::SetWindowSize(int width, int height) {
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
    return true;
  }

  if (!SDL_SetWindowSize(window_, width, height)) {
    return false;
  }

  if (!UpdateWindowMetrics()) {
    return false;
  }

  if (graphics_ready_) {
    swapchain_dirty_ = true;
  }

  log::Info("renderer", "window size set to {}x{} canvas={}x{} scale={:.2f}x{:.2f}",
            window_width_,
            window_height_,
            canvas_width_,
            canvas_height_,
            canvas_scale_x_,
            canvas_scale_y_);
  return true;
}

} // namespace luna
