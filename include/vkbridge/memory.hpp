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
    bool coherent_ = true;
};

} // namespace vkbridge
