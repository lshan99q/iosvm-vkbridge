// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vkbridge/status.hpp"

namespace vkbridge
{
// Tightly packed RGBA8 image. The PNG writer deliberately supports only this
// format, and writes it with stored (uncompressed) deflate blocks so no zlib
// dependency is needed.
struct PngImage {
    const uint8_t *rgba = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    // Bytes between the start of two rows. Zero means width * 4.
    uint32_t stride_bytes = 0;

    uint32_t row_bytes() const
    {
        return width * 4;
    }
};

Status encode_png(std::vector<uint8_t> &out, const PngImage &image);
Status write_png(const std::string &path, const PngImage &image);

} // namespace vkbridge
