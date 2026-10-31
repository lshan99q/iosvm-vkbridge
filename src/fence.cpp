// SPDX-License-Identifier: MIT

#include "vkbridge/fence.hpp"

#include "vkbridge/vk_result.hpp"

namespace vkbridge
{

Fence::Fence(Fence &&other) noexcept
    : device_(other.device_), fence_(other.fence_), sequence_(other.sequence_)
{
    other.device_ = VK_NULL_HANDLE;
    other.fence_ = VK_NULL_HANDLE;
    other.sequence_ = 0;
}

Fence &Fence::operator=(Fence &&other) noexcept
{
    if (this != &other) {
        destroy();
        device_ = other.device_;
        fence_ = other.fence_;
        sequence_ = other.sequence_;
        other.device_ = VK_NULL_HANDLE;
        other.fence_ = VK_NULL_HANDLE;
        other.sequence_ = 0;
    }
    return *this;
}

Fence::~Fence()
{
    destroy();
}

Result<Fence> Fence::create(VkDevice device, uint64_t sequence)
{
    if (device == VK_NULL_HANDLE) {
        return err_result<Fence>(Error::InvalidArgument, "fence needs a valid VkDevice");
    }
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    const Status status = vk_status(vkCreateFence(device, &info, nullptr, &fence), "vkCreateFence");
    if (!status) {
        return err_result<Fence>(status);
    }
    Fence result;
    result.device_ = device;
    result.fence_ = fence;
    result.sequence_ = sequence;
    return ok_result<Fence>(std::move(result));
}

Status Fence::wait(uint64_t timeout_ns) const
{
    if (fence_ == VK_NULL_HANDLE) {
        return status_fail(Error::InvalidArgument, "fence is not valid");
    }
    const VkResult result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
    if (result == VK_TIMEOUT) {
        return status_fail(Error::Timeout, "fence " + std::to_string(sequence_) + " not signaled");
    }
    return vk_status(result, "vkWaitForFences");
}

Status Fence::reset()
{
    if (fence_ == VK_NULL_HANDLE) {
        return status_fail(Error::InvalidArgument, "fence is not valid");
    }
    return vk_status(vkResetFences(device_, 1, &fence_), "vkResetFences");
}

Result<bool> Fence::signaled() const
{
    if (fence_ == VK_NULL_HANDLE) {
        return err_result<bool>(Error::InvalidArgument, "fence is not valid");
    }
    const VkResult result = vkGetFenceStatus(device_, fence_);
    if (result == VK_SUCCESS) {
        return ok_result(true);
    }
    if (result == VK_NOT_READY) {
        return ok_result(false);
    }
    return err_result<bool>(vk_status(result, "vkGetFenceStatus"));
}

VkFence Fence::release() noexcept
{
    const VkFence fence = fence_;
    destroy_handle_only();
    return fence;
}

void Fence::destroy_handle_only() noexcept
{
    device_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    sequence_ = 0;
}

void Fence::destroy() noexcept
{
    if (fence_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, fence_, nullptr);
    }
    destroy_handle_only();
}

} // namespace vkbridge
