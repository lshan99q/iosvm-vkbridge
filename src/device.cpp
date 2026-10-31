// SPDX-License-Identifier: MIT

#include "vkbridge/device.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "vkbridge/vk_result.hpp"

namespace vkbridge
{
namespace
{

bool layer_available(const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }
    std::vector<VkLayerProperties> properties(count);
    if (vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkLayerProperties &property : properties) {
        if (std::strcmp(property.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool extension_available(const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return false;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties &property : properties) {
        if (std::strcmp(property.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_messenger_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
    const char *level =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 ? "error" : "warning";
    std::fprintf(stderr, "[vulkan %s] %s\n", level,
                 data->pMessage == nullptr ? "" : data->pMessage);
    return VK_FALSE;
}

std::string lower_ascii(const std::string &text)
{
    std::string lowered = text;
    for (char &character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return lowered;
}

struct QueueChoice {
    uint32_t family = 0;
    VkQueueFlags flags = 0;
    uint32_t count = 0;
    int score = -1;
};

QueueChoice choose_queue_family(VkPhysicalDevice physical_device)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    if (count > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families.data());
    }

    QueueChoice best;
    for (uint32_t index = 0; index < count; ++index) {
        if (families[index].queueCount == 0) {
            continue;
        }
        const VkQueueFlags flags = families[index].queueFlags;
        const bool graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
        int score = -1;
        if (graphics && compute) {
            score = 5;
        } else if (graphics) {
            score = 4;
        } else if (compute) {
            score = 2;
        }
        if (score > best.score) {
            best.family = index;
            best.flags = flags;
            best.count = families[index].queueCount;
            best.score = score;
        }
    }
    return best;
}

int device_type_score(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM:
        return 0;
    }
    return 0;
}

std::string describe_devices(const std::vector<VkPhysicalDevice> &devices)
{
    std::string description;
    for (const VkPhysicalDevice physical : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (!description.empty()) {
            description += ", ";
        }
        description += properties.deviceName;
    }
    return description;
}

struct Candidate {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    QueueChoice queue;
    int score = 0;
};

} // namespace

Instance::Instance(Instance &&other) noexcept
    : instance_(other.instance_), debug_messenger_(other.debug_messenger_),
      destroy_debug_messenger_(other.destroy_debug_messenger_),
      physical_devices_(std::move(other.physical_devices_))
{
    other.instance_ = VK_NULL_HANDLE;
    other.debug_messenger_ = VK_NULL_HANDLE;
    other.destroy_debug_messenger_ = nullptr;
}

Instance &Instance::operator=(Instance &&other) noexcept
{
    if (this != &other) {
        destroy();
        instance_ = other.instance_;
        debug_messenger_ = other.debug_messenger_;
        destroy_debug_messenger_ = other.destroy_debug_messenger_;
        physical_devices_ = std::move(other.physical_devices_);
        other.instance_ = VK_NULL_HANDLE;
        other.debug_messenger_ = VK_NULL_HANDLE;
        other.destroy_debug_messenger_ = nullptr;
    }
    return *this;
}

Instance::~Instance()
{
    destroy();
}

void Instance::destroy() noexcept
{
    if (debug_messenger_ != VK_NULL_HANDLE && destroy_debug_messenger_ != nullptr) {
        destroy_debug_messenger_(instance_, debug_messenger_, nullptr);
    }
    debug_messenger_ = VK_NULL_HANDLE;
    destroy_debug_messenger_ = nullptr;
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
    physical_devices_.clear();
}

Result<Instance> Instance::create(const InstanceOptions &options)
{
    Instance instance;

    const bool want_validation = options.enable_validation;
    std::vector<const char *> layers;
    std::vector<const char *> extensions;
    if (want_validation) {
        if (!layer_available("VK_LAYER_KHRONOS_validation")) {
            return err_result<Instance>(
                Error::Unsupported,
                "validation layer VK_LAYER_KHRONOS_validation is not installed");
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
        if (extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = options.application_name;
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "IOSVM";
    application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;
    create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    VkInstance handle = VK_NULL_HANDLE;
    const Status created =
        vk_status(vkCreateInstance(&create_info, nullptr, &handle), "vkCreateInstance");
    if (!created) {
        return err_result<Instance>(created);
    }
    instance.instance_ = handle;

    uint32_t device_count = 0;
    VkResult result = vkEnumeratePhysicalDevices(handle, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
        instance.destroy();
        return err_result<Instance>(Error::NoDevice, "the instance enumerated no Vulkan devices");
    }
    instance.physical_devices_.resize(device_count);
    result = vkEnumeratePhysicalDevices(handle, &device_count, instance.physical_devices_.data());
    if (result != VK_SUCCESS) {
        const Status status = vk_status(result, "vkEnumeratePhysicalDevices");
        instance.destroy();
        return err_result<Instance>(status);
    }
    instance.physical_devices_.resize(device_count);

    if (want_validation && !extensions.empty()) {
        const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(handle, "vkCreateDebugUtilsMessengerEXT"));
        instance.destroy_debug_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(handle, "vkDestroyDebugUtilsMessengerEXT"));
        if (create_messenger == nullptr || instance.destroy_debug_messenger_ == nullptr) {
            instance.destroy();
            return err_result<Instance>(Error::Unsupported,
                                        "VK_EXT_debug_utils entry points are missing");
        }
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = debug_messenger_callback;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        result = create_messenger(handle, &messenger_info, nullptr, &messenger);
        if (result != VK_SUCCESS) {
            const Status status = vk_status(result, "vkCreateDebugUtilsMessengerEXT");
            instance.destroy();
            return err_result<Instance>(status);
        }
        instance.debug_messenger_ = messenger;
    }

    return ok_result<Instance>(std::move(instance));
}

Device::Device(Device &&other) noexcept
    : physical_device_(other.physical_device_), device_(other.device_), queue_(other.queue_),
      queue_family_(other.queue_family_), info_(std::move(other.info_)),
      memory_properties_(other.memory_properties_)
{
    other.physical_device_ = VK_NULL_HANDLE;
    other.device_ = VK_NULL_HANDLE;
    other.queue_ = VK_NULL_HANDLE;
    other.queue_family_ = 0;
    other.info_ = DeviceInfo{};
    other.memory_properties_ = VkPhysicalDeviceMemoryProperties{};
}

Device &Device::operator=(Device &&other) noexcept
{
    if (this != &other) {
        destroy();
        physical_device_ = other.physical_device_;
        device_ = other.device_;
        queue_ = other.queue_;
        queue_family_ = other.queue_family_;
        info_ = std::move(other.info_);
        memory_properties_ = other.memory_properties_;
        other.physical_device_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.queue_ = VK_NULL_HANDLE;
        other.queue_family_ = 0;
        other.info_ = DeviceInfo{};
        other.memory_properties_ = VkPhysicalDeviceMemoryProperties{};
    }
    return *this;
}

Device::~Device()
{
    destroy();
}

void Device::destroy() noexcept
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    queue_family_ = 0;
    info_ = DeviceInfo{};
    memory_properties_ = VkPhysicalDeviceMemoryProperties{};
}

Status Device::wait_idle() const
{
    if (device_ == VK_NULL_HANDLE) {
        return status_fail(Error::InvalidArgument, "device is not valid");
    }
    return vk_status(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
}

Result<Device> Device::create(Instance &instance, const DeviceOptions &options)
{
    if (!instance.valid()) {
        return err_result<Device>(Error::InvalidArgument, "instance is not valid");
    }
    const std::vector<VkPhysicalDevice> &physicals = instance.physical_devices();
    if (physicals.empty()) {
        return err_result<Device>(Error::NoDevice, "the instance enumerated no Vulkan devices");
    }

    const std::string needle = lower_ascii(options.device_substring);
    std::vector<Candidate> candidates;
    for (const VkPhysicalDevice physical : physicals) {
        Candidate candidate;
        candidate.physical = physical;
        vkGetPhysicalDeviceProperties(physical, &candidate.properties);
        candidate.queue = choose_queue_family(physical);
        if (candidate.queue.score < 0) {
            continue;
        }
        const std::string name = lower_ascii(candidate.properties.deviceName);
        if (!needle.empty() && name.find(needle) == std::string::npos) {
            continue;
        }
        candidate.score =
            device_type_score(candidate.properties.deviceType) * 10 + candidate.queue.score;
        candidates.push_back(candidate);
    }

    if (candidates.empty()) {
        std::string detail;
        if (needle.empty()) {
            detail = "no physical device exposes a graphics or compute queue";
        } else {
            detail = "no device name contains \"" + options.device_substring +
                     "\"; available: " + describe_devices(physicals);
        }
        return err_result<Device>(Error::NoDevice, detail);
    }

    size_t chosen = 0;
    for (size_t index = 1; index < candidates.size(); ++index) {
        if (candidates[index].score > candidates[chosen].score) {
            chosen = index;
        }
    }
    const Candidate &best = candidates[chosen];

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = best.queue.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;

    VkDevice handle = VK_NULL_HANDLE;
    const Status created =
        vk_status(vkCreateDevice(best.physical, &create_info, nullptr, &handle), "vkCreateDevice");
    if (!created) {
        return err_result<Device>(
            Status(Error::DeviceCreateFailed,
                   std::string("\"") + best.properties.deviceName + "\": " + created.to_string()));
    }

    Device device;
    device.physical_device_ = best.physical;
    device.device_ = handle;
    device.queue_family_ = best.queue.family;
    vkGetDeviceQueue(handle, best.queue.family, 0, &device.queue_);
    vkGetPhysicalDeviceMemoryProperties(best.physical, &device.memory_properties_);

    device.info_.name = best.properties.deviceName;
    device.info_.vendor_id = best.properties.vendorID;
    device.info_.device_id = best.properties.deviceID;
    device.info_.api_version = best.properties.apiVersion;
    device.info_.driver_version = best.properties.driverVersion;
    device.info_.type = best.properties.deviceType;
    device.info_.queue_family = best.queue.family;
    device.info_.queue_count = best.queue.count;
    device.info_.queue_flags = best.queue.flags;
    device.info_.non_coherent_atom_size = std::max<VkDeviceSize>(
        1, static_cast<VkDeviceSize>(best.properties.limits.nonCoherentAtomSize));

    if (best.properties.apiVersion >= VK_API_VERSION_1_1) {
        VkPhysicalDeviceMaintenance3Properties maintenance{};
        maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &maintenance;
        vkGetPhysicalDeviceProperties2(best.physical, &properties2);
        device.info_.max_buffer_bytes = maintenance.maxMemoryAllocationSize;
    }

    return ok_result<Device>(std::move(device));
}

} // namespace vkbridge
