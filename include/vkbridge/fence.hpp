// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "vkbridge/status.hpp"

namespace vkbridge
{
// RAII VkFence carrying the submission sequence number it was submitted with, so
// completion can be matched to submission order.
class Fence
{
  public:
    Fence() = default;
    Fence(const Fence &) = delete;
    Fence &operator=(const Fence &) = delete;
    Fence(Fence &&other) noexcept;
    Fence &operator=(Fence &&other) noexcept;
    ~Fence();

    static Result<Fence> create(VkDevice device, uint64_t sequence);

    bool valid() const
    {
        return fence_ != VK_NULL_HANDLE;
    }
    VkFence handle() const
    {
        return fence_;
    }
    uint64_t sequence() const
    {
        return sequence_;
    }

    // Blocks until signaled or the timeout (nanoseconds) expires.
    Status wait(uint64_t timeout_ns) const;
    Status reset();

    // Never blocks. False means not signaled yet.
    Result<bool> signaled() const;

  private:
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    uint64_t sequence_ = 0;
};

} // namespace vkbridge
