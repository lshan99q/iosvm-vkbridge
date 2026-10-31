// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkbridge/device.hpp"
#include "vkbridge/memory.hpp"
#include "vkbridge/status.hpp"

namespace vkbridge
{
struct ScreenshotOptions {
    uint32_t width = 320;
    uint32_t height = 200;
};

struct ScreenshotImage {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t unique_colours = 0;
    std::vector<uint8_t> rgba;
};

// Renders the deterministic test pattern into an offscreen colour image with a
// real render pass (vkCmdClearAttachments rectangles, no shaders yet), copies it
// back through the staging arena and returns tightly packed RGBA8 pixels.
Result<ScreenshotImage> render_screenshot(Device &device, VkCommandPool command_pool,
                                          StagingArena &staging, const ScreenshotOptions &options);

} // namespace vkbridge
