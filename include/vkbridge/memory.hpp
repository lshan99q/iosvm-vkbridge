// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "vkbridge/device.hpp"
#include "vkbridge/status.hpp"

namespace vkbridge
{
enum class MemoryUse {
    // Device-local heap: the buffers that the GPU actually reads and writes.
    DeviceLocal,
    // Host-visible heap: staging for uploads and readbacks.
    HostVisible,
};

struct MemoryBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t type_index = 0;
    void *mapped = nullptr;
};

struct BufferObject {
    VkBuffer buffer = VK_NULL_HANDLE;
    MemoryBlock memory;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
};

// Dedicated allocation (one VkDeviceMemory per resource); no suballocation yet.
Result<MemoryBlock> allocate_memory(Device &device, const VkMemoryRequirements &requirements,
                                    MemoryUse use);
Status free_memory(Device &device, MemoryBlock &block) noexcept;

// Derived description of one VkPhysicalDeviceMemoryProperties entry. The probe prints
// it in --verbose so a reader can see which heap a buffer landed in.
struct MemoryTypeInfo {
    VkMemoryPropertyFlags flags = 0;
    VkMemoryHeapFlags heap_flags = 0;
    uint32_t heap_index = 0;
    VkDeviceSize heap_size = 0;
    bool host_visible = false;
    bool device_local = false;
    bool host_coherent = false;
    bool host_cached = false;
};

// Fills `info` for a memory type index. Fails with Error::NotFound when the index is
// outside the device's memory type count.
Status memory_type_info(const Device &device, uint32_t type_index, MemoryTypeInfo &info);

// True when memory of this type can be mapped by the host.
bool memory_type_is_host_visible(const Device &device, uint32_t type_index);

// True when memory of this type is on a device-local heap.
bool memory_type_is_device_local(const Device &device, uint32_t type_index);

// Creates a buffer with TRANSFER_SRC | TRANSFER_DST forced on, because every
// buffer must be reachable by the staging copy path.
Result<BufferObject> allocate_buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
                                     MemoryUse use);
Status release_buffer(Device &device, BufferObject &buffer) noexcept;

// One persistent host-visible staging buffer, grown on demand. Host transfers are
// always bracketed by submit + fence wait, so the whole buffer is reusable for
// each operation; there is no ring or per-in-flight-slot partitioning.
class StagingArena
{
  public:
    explicit StagingArena(Device &device);
    StagingArena(const StagingArena &) = delete;
    StagingArena &operator=(const StagingArena &) = delete;
    StagingArena(StagingArena &&other) noexcept;
    StagingArena &operator=(StagingArena &&other) noexcept;
    ~StagingArena();

    // Ensures at least `bytes` of capacity and returns the mapped base pointer.
    Result<void *> ensure(VkDeviceSize bytes);

    VkBuffer buffer() const
    {
        return buffer_;
    }
    VkDeviceSize capacity() const
    {
        return capacity_;
    }
    void *host_pointer() const
    {
        return mapped_;
    }
    bool coherent() const
    {
        return coherent_;
    }
    uint32_t memory_type_index() const
    {
        return memory_type_index_;
    }

    Status flush_all();
    Status invalidate_all();
    void destroy() noexcept;

  private:
    void take_from(StagingArena &other) noexcept;

    Device *device_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize capacity_ = 0;
    void *mapped_ = nullptr;
    uint32_t memory_type_index_ = 0;
    bool coherent_ = true;
};

} // namespace vkbridge
