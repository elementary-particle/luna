#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <skia/gpu/vk/VulkanMemoryAllocator.h>

namespace {

class VulkanMemoryAllocator : public skgpu::VulkanMemoryAllocator {
public:
  explicit VulkanMemoryAllocator(VmaAllocator allocator)
      : allocator_(allocator) {}
  ~VulkanMemoryAllocator() override { vmaDestroyAllocator(allocator_); }

  VkResult
  allocateImageMemory(VkImage image, uint32_t allocation_property_flags,
                      skgpu::VulkanBackendMemory *backend_memory) override {
    VmaAllocationCreateInfo info;
    info.flags = 0;
    info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    info.preferredFlags = 0;
    info.memoryTypeBits = 0;
    info.pool = VK_NULL_HANDLE;
    info.pUserData = nullptr;

    if (kDedicatedAllocation_AllocationPropertyFlag &
        allocation_property_flags) {
      info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if (kLazyAllocation_AllocationPropertyFlag & allocation_property_flags) {
      info.requiredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    }
    if (kProtected_AllocationPropertyFlag & allocation_property_flags) {
      info.requiredFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;
    }

    VmaAllocation allocation;
    VkResult result = vmaAllocateMemoryForImage(
        allocator_, image, &info, &allocation, nullptr);
    if (VK_SUCCESS == result) {
      *backend_memory = (skgpu::VulkanBackendMemory)allocation;
    }
    return result;
  }

  VkResult
  allocateBufferMemory(VkBuffer buffer, BufferUsage usage,
                       uint32_t allocation_property_flags,
                       skgpu::VulkanBackendMemory *backend_memory) override {
    VmaAllocationCreateInfo info;
    info.flags = 0;
    info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    info.memoryTypeBits = 0;
    info.pool = VK_NULL_HANDLE;
    info.pUserData = nullptr;

    switch (usage) {
    case BufferUsage::kGpuOnly:
      info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      info.preferredFlags = 0;
      break;
    case BufferUsage::kCpuWritesGpuReads:
      // When doing cpu writes and gpu reads the general rule of thumb is to use
      // coherent memory. Though this depends on the fact that we are not doing
      // any cpu reads and the cpu writes are sequential. For sparse writes we'd
      // want cpu cached memory, however we don't do these types of writes in
      // Skia.
      //
      // TODO: In the future there may be times where specific types of memory
      // could benefit from a coherent and cached memory. Typically these allow
      // for the gpu to read cpu writes from the cache without needing to flush
      // the writes throughout the cache. The reverse is not true and GPU writes
      // tend to invalidate the cache regardless. Also these gpu cache read
      // access are typically lower bandwidth than non-cached memory. For now
      // Skia doesn't really have a need or want of this type of memory. But if
      // we ever do we could pass in an AllocationPropertyFlag that requests the
      // cached property.
      info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      break;
    case BufferUsage::kTransfersFromCpuToGpu:
      info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      info.preferredFlags = 0;
      break;
    case BufferUsage::kTransfersFromGpuToCpu:
      info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      break;
    }

    if (kDedicatedAllocation_AllocationPropertyFlag &
        allocation_property_flags) {
      info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if ((kLazyAllocation_AllocationPropertyFlag & allocation_property_flags) &&
        BufferUsage::kGpuOnly == usage) {
      info.preferredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    }

    if (kPersistentlyMapped_AllocationPropertyFlag &
        allocation_property_flags) {
      SkASSERT(BufferUsage::kGpuOnly != usage);
      info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    if (kProtected_AllocationPropertyFlag & allocation_property_flags) {
      info.requiredFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;
    }

    VmaAllocation allocation;
    VkResult result = vmaAllocateMemoryForBuffer(
        allocator_, buffer, &info, &allocation, nullptr);
    if (VK_SUCCESS == result) {
      *backend_memory = (skgpu::VulkanBackendMemory)allocation;
    }

    return result;
  }

  void freeMemory(const skgpu::VulkanBackendMemory &memory_handle) override {
    {
      const VmaAllocation allocation = (VmaAllocation)memory_handle;
      vmaFreeMemory(allocator_, allocation);
    }
  }

  void getAllocInfo(const skgpu::VulkanBackendMemory &memory_handle,
                    skgpu::VulkanAlloc *alloc) const override {
    const VmaAllocation allocation = (VmaAllocation)memory_handle;
    VmaAllocationInfo vmaInfo;
    vmaGetAllocationInfo(allocator_, allocation, &vmaInfo);

    VkMemoryPropertyFlags mem_flags;
    vmaGetMemoryTypeProperties(allocator_, vmaInfo.memoryType, &mem_flags);

    uint32_t flags = 0;
    if (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT & mem_flags) {
      flags |= skgpu::VulkanAlloc::kMappable_Flag;
    }
    if (!SkToBool(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT & mem_flags)) {
      flags |= skgpu::VulkanAlloc::kNoncoherent_Flag;
    }
    if (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT & mem_flags) {
      flags |= skgpu::VulkanAlloc::kLazilyAllocated_Flag;
    }

    alloc->fMemory = vmaInfo.deviceMemory;
    alloc->fOffset = vmaInfo.offset;
    alloc->fSize = vmaInfo.size;
    alloc->fFlags = flags;
    alloc->fBackendMemory = memory_handle;
  }

  VkResult mapMemory(const skgpu::VulkanBackendMemory &memory_handle,
                     void **data) override {
    {
      const VmaAllocation allocation = (VmaAllocation)memory_handle;
      return vmaMapMemory(allocator_, allocation, data);
    }
  }
  void unmapMemory(const skgpu::VulkanBackendMemory &memory_handle) override {
    const VmaAllocation allocation = (VmaAllocation)memory_handle;
    vmaUnmapMemory(allocator_, allocation);
  }

  VkResult flushMemory(const skgpu::VulkanBackendMemory &memory_handle,
                       VkDeviceSize offset, VkDeviceSize size) override {
    const VmaAllocation allocation = (VmaAllocation)memory_handle;
    return vmaFlushAllocation(allocator_, allocation, offset, size);
  }
  VkResult invalidateMemory(const skgpu::VulkanBackendMemory &memory_handle,
                            VkDeviceSize offset, VkDeviceSize size) override {
    const VmaAllocation allocation = (VmaAllocation)memory_handle;
    return vmaInvalidateAllocation(allocator_, allocation, offset, size);
  }

  std::pair<uint64_t, uint64_t> totalAllocatedAndUsedMemory() const override {
    VmaTotalStatistics stats;
    vmaCalculateStatistics(allocator_, &stats);
    return {stats.total.statistics.blockBytes,
            stats.total.statistics.allocationBytes};
  }

private:
  VmaAllocator allocator_;
};

} // namespace

sk_sp<skgpu::VulkanMemoryAllocator>
MakeVulkanMemoryAllocator(VkInstance instance, VkPhysicalDevice physical_device,
                          VkDevice device,
                          PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr,
                          PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr) {
  VmaVulkanFunctions functions = {};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo allocator_info = {};
  allocator_info.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
  allocator_info.physicalDevice = physical_device;
  allocator_info.device = device;
  allocator_info.instance = instance;
  allocator_info.pVulkanFunctions = &functions;

  VmaAllocator allocator;
  vmaCreateAllocator(&allocator_info, &allocator);

  return sk_sp<VulkanMemoryAllocator>(new VulkanMemoryAllocator(allocator));
}
