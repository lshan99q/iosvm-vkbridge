// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkbridge/status.hpp"
#include "vkbridge/vk_result.hpp"

namespace vkbridge
{
struct InstanceOptions {
    const char *application_name = "vkbridge";
    bool enable_validation = false;
};

// Owns VkInstance and the enumerated physical devices. Headless: no surface or
// window-system extension is requested or required. Must outlive the Device made
// from it.
class Instance
{
  public:
    static Result<Instance> create(const InstanceOptions &options);

    Instance() = default;
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;
    Instance(Instance &&other) noexcept;
    Instance &operator=(Instance &&other) noexcept;
    ~Instance();

    VkInstance handle() const
    {
        return instance_;
    }
    bool valid() const
    {
        return instance_ != VK_NULL_HANDLE;
    }
    const std::vector<VkPhysicalDevice> &physical_devices() const
    {
        return physical_devices_;
    }
    bool validation_enabled() const
    {
        return debug_messenger_ != VK_NULL_HANDLE;
    }

  private:
    void destroy() noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger_ = nullptr;
    std::vector<VkPhysicalDevice> physical_devices_;
};

struct DeviceOptions {
    // Case-insensitive substring matched against VkPhysicalDeviceProperties::deviceName.
    // Empty selects the best available device (discrete GPU first, CPU last).
    std::string device_substring;
};

struct DeviceInfo {
    std::string name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t queue_family = 0;
    uint32_t queue_count = 0;
    VkQueueFlags queue_flags = 0;
    VkDeviceSize max_buffer_bytes = 0;
    VkDeviceSize non_coherent_atom_size = 1;
    // VkPhysicalDeviceLimits::timestampPeriod, in nanoseconds per tick.
    float timestamp_period_ns = 1.0f;
    // Timestamp bits of the selected queue family; zero means device timestamps are
    // not available on this queue.
    uint32_t timestamp_valid_bits = 0;
};

// Owns VkDevice plus one queue from a graphics-or-compute family. No optional
// features, extensions or multiple queues are enabled.
class Device
{
  public:
    static Result<Device> create(Instance &instance, const DeviceOptions &options);

    Device() = default;
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;
    Device(Device &&other) noexcept;
    Device &operator=(Device &&other) noexcept;
    ~Device();

    VkPhysicalDevice physical_device() const
    {
        return physical_device_;
    }
    VkDevice handle() const
    {
        return device_;
    }
    VkQueue queue() const
    {
        return queue_;
    }
    uint32_t queue_family() const
    {
        return queue_family_;
    }
    VkQueueFlags queue_flags() const
    {
        return info_.queue_flags;
    }
    bool graphics_capable() const
    {
        return (info_.queue_flags & VK_QUEUE_GRAPHICS_BIT) != 0;
    }
    const DeviceInfo &info() const
    {
        return info_;
    }
    const VkPhysicalDeviceMemoryProperties &memory_properties() const
    {
        return memory_properties_;
    }
    Status wait_idle() const;

  private:
    void destroy() noexcept;

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    DeviceInfo info_;
    VkPhysicalDeviceMemoryProperties memory_properties_{};
};

} // namespace vkbridge
