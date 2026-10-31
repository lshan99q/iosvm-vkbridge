// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkbridge/device.hpp"
#include "vkbridge/memory.hpp"
#include "vkbridge/oracle.hpp"
#include "vkbridge/status.hpp"

namespace vkbridge
{
struct ScreenshotOptions {
    uint32_t width = 320;
    uint32_t height = 200;
    // Mixed into the device-side attestation words. A stale attestation from an earlier
    // frame cannot satisfy a check computed with a different nonce.
    uint32_t attestation_nonce = 0;
};

struct ScreenshotImage {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t unique_colours = 0;
    std::vector<uint8_t> rgba;
    // Words written by the oracle shader after it read the rendered image on the
    // device: {checksum ^ nonce, kAttestationFingerprint ^ nonce}. A host-only
    // replacement of this function cannot produce them.
    Attestation attestation;
    bool render_target_device_local = false;
    bool render_target_host_visible = false;
};

// Renders the deterministic test pattern into an offscreen colour image with a
// real render pass (vkCmdClearAttachments rectangles, no draw calls), copies it
// back through the staging arena, runs the compute-shader attestation over the
// device-local copy and returns tightly packed RGBA8 pixels.
Result<ScreenshotImage> render_screenshot(Device &device, VkCommandPool command_pool,
                                          StagingArena &staging, const ScreenshotOptions &options);

} // namespace vkbridge
