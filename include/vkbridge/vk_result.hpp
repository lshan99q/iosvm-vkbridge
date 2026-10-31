// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

#include <vulkan/vulkan.h>

#include "vkbridge/status.hpp"

namespace vkbridge
{
const char *vk_result_name(VkResult result);

// Maps a VkResult to a Status, keeping the failing call name in the detail.
Status vk_status(VkResult result, std::string_view what);

const char *vk_physical_device_type_name(VkPhysicalDeviceType type);

} // namespace vkbridge
