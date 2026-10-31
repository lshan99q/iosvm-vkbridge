// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkbridge/status.hpp"

namespace vkbridge
{
enum class ObjectKind : uint32_t {
    Buffer = 1,
    Image = 2,
};

// One guest-visible resource. The table itself never calls Vulkan: it only owns
// the mapping handle -> record, which keeps it unit-testable without a GPU. The
// caller (Replay) is responsible for creating and destroying the Vulkan objects.
struct ObjectRecord {
    uint32_t handle = 0;
    ObjectKind kind = ObjectKind::Buffer;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    // VkBufferUsageFlags for buffers, VkImageUsageFlags for images.
    uint32_t usage = 0;
    // Memory type the allocation came from, plus the two properties the probe asserts
    // on: a blit source/destination must be device-local, and on devices that expose a
    // non-mappable device-local type it must not be host-visible at all.
    uint32_t memory_type_index = 0;
    bool host_visible = false;
    bool device_local = false;
};

class ObjectTable
{
  public:
    static constexpr uint32_t kInvalidHandle = 0;

    explicit ObjectTable(size_t max_objects = 4096);

    // Allocates a fresh monotonically increasing handle. Handles are never
    // reused, so a stale handle can never alias a live object.
    Result<uint32_t> add(const ObjectRecord &record);

    // Inserts under a caller-provided non-zero handle. Intended for replaying a
    // guest-side handle space once the wire format exists.
    Result<uint32_t> add_with_handle(uint32_t handle, const ObjectRecord &record);

    const ObjectRecord *find(uint32_t handle) const;
    ObjectRecord *find(uint32_t handle);
    bool contains(uint32_t handle) const;

    // Removes and returns the record so the caller can free its Vulkan objects.
    Result<ObjectRecord> remove(uint32_t handle);

    // Removes everything; used by Replay when tearing down.
    std::vector<ObjectRecord> take_all();

    size_t size() const
    {
        return objects_.size();
    }
    size_t max_objects() const
    {
        return max_objects_;
    }
    uint32_t next_handle() const
    {
        return next_handle_;
    }

  private:
    Result<uint32_t> insert(uint32_t handle, const ObjectRecord &record);

    std::unordered_map<uint32_t, ObjectRecord> objects_;
    uint32_t next_handle_ = 1;
    size_t max_objects_ = 4096;
};

} // namespace vkbridge
