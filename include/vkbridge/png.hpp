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

// Pixels decoded from the PNG subset the writer emits.
struct PngPixels {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t stored_blocks = 0;
    // Tightly packed RGBA8, width * height * 4 bytes.
    std::vector<uint8_t> rgba;
};

// Decodes and validates a PNG produced by encode_png: signature, every chunk CRC, the
// IHDR fields, the zlib header, the stored-deflate block framing and the Adler-32. Only
// the format the writer emits is accepted (8-bit RGBA, filter type 0, no interlacing);
// anything else fails rather than being guessed at.
Status decode_png(const uint8_t *bytes, size_t size, PngPixels &out);

// Reads `path` and decodes it with decode_png().
Status read_png(const std::string &path, PngPixels &out);

} // namespace vkbridge
