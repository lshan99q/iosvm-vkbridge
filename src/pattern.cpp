// SPDX-License-Identifier: MIT

#include "vkbridge/pattern.hpp"

#include <algorithm>

namespace vkbridge
{
namespace
{

constexpr uint32_t kBackground = 0x080C14FFu;

// Deliberately pairwise distinct colours so the expected unique-colour count is
// simply 1 + 8 + 5 + 2 + 6 == 22 for the default 320x200 frame.
constexpr uint32_t kBands[8] = {
    0xE04040FFu, 0xE08020FFu, 0xE0C030FFu, 0x60C050FFu,
    0x30C0B0FFu, 0x3080E0FFu, 0x7060E0FFu, 0xC050C0FFu,
};
constexpr uint32_t kStripes[5] = {
    0xF0F0F0FFu, 0x606060FFu, 0x204020FFu, 0x402040FFu, 0x202060FFu,
};
constexpr uint32_t kChecker[2] = {0x000000FFu, 0xFF00C0FFu};
constexpr uint32_t kStairs[6] = {
    0xFFD080FFu, 0x80FFD0FFu, 0xD080FFFFu, 0xFF8080FFu, 0x80D0FFFFu, 0xD0FF80FFu,
};

inline void store_rgba(uint8_t *pixel, uint32_t rgba)
{
    pixel[0] = static_cast<uint8_t>((rgba >> 24) & 0xFFu);
    pixel[1] = static_cast<uint8_t>((rgba >> 16) & 0xFFu);
    pixel[2] = static_cast<uint8_t>((rgba >> 8) & 0xFFu);
    pixel[3] = static_cast<uint8_t>(rgba & 0xFFu);
}

} // namespace

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) |
           (static_cast<uint32_t>(b) << 8) | static_cast<uint32_t>(a);
}

uint32_t read_le32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value & 0xFFu);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    bytes[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t upload_word(uint64_t word_index)
{
    uint64_t x = word_index * 0x9E3779B97F4A7C15ull + 0x1234ABCDull;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return static_cast<uint32_t>(x >> 16);
}

std::vector<uint8_t> make_upload_pattern(size_t bytes)
{
    std::vector<uint8_t> out(bytes, 0);
    const size_t words = (bytes + 3) / 4;
    for (size_t word = 0; word < words; ++word) {
        const uint32_t value = upload_word(word);
        const size_t base = word * 4;
        for (size_t byte = 0; byte < 4 && base + byte < bytes; ++byte) {
            out[base + byte] = static_cast<uint8_t>((value >> (8 * byte)) & 0xFFu);
        }
    }
    return out;
}

bool verify_upload_pattern(const uint8_t *data, size_t bytes)
{
    const size_t words = (bytes + 3) / 4;
    for (size_t word = 0; word < words; ++word) {
        const uint32_t expected = upload_word(word);
        const size_t base = word * 4;
        for (size_t byte = 0; byte < 4 && base + byte < bytes; ++byte) {
            if (data[base + byte] != static_cast<uint8_t>((expected >> (8 * byte)) & 0xFFu)) {
                return false;
            }
        }
    }
    return true;
}

bool verify_fill(const uint8_t *data, size_t bytes, uint32_t value32)
{
    if (bytes % 4 != 0) {
        return false;
    }
    const size_t words = bytes / 4;
    for (size_t word = 0; word < words; ++word) {
        if (read_le32(data + word * 4) != value32) {
            return false;
        }
    }
    return true;
}

ScreenshotLayout make_screenshot_layout(uint32_t width, uint32_t height)
{
    ScreenshotLayout layout;
    layout.width = width;
    layout.height = height;
    layout.background_rgba = kBackground;
    if (width == 0 || height == 0) {
        return layout;
    }

    // Leave a background margin above and below so the clear colour survives.
    const uint32_t band_y = height / 20;
    const uint32_t band_height = height - 2 * band_y;
    constexpr uint32_t kBandCount = 8;
    if (band_height > 0) {
        for (uint32_t i = 0; i < kBandCount; ++i) {
            const uint32_t x0 =
                static_cast<uint32_t>((static_cast<uint64_t>(width) * i) / kBandCount);
            const uint32_t x1 =
                static_cast<uint32_t>((static_cast<uint64_t>(width) * (i + 1)) / kBandCount);
            if (x1 > x0) {
                layout.rects.push_back({x0, band_y, x1 - x0, band_height, kBands[i]});
            }
        }
    }

    constexpr uint32_t kStripeCount = 5;
    uint32_t stripe_height = height / 24;
    if (stripe_height == 0) {
        stripe_height = 1;
    }
    for (uint32_t i = 0; i < kStripeCount; ++i) {
        const uint32_t y =
            static_cast<uint32_t>((static_cast<uint64_t>(height) * (i + 1)) / (kStripeCount + 1));
        if (y >= height) {
            continue;
        }
        const uint32_t h = std::min(stripe_height, height - y);
        layout.rects.push_back({0, y, width, h, kStripes[i]});
    }

    // Checkerboard in the lower-right quadrant.
    const uint32_t region_x = width / 2;
    const uint32_t region_y = height / 2;
    const uint32_t region_w = width - region_x;
    const uint32_t region_h = height - region_y;
    uint32_t cell = std::min(width, height) / 20;
    if (cell < 2) {
        cell = 2;
    }
    if (region_w > 0 && region_h > 0) {
        const uint32_t cols = (region_w + cell - 1) / cell;
        const uint32_t rows = (region_h + cell - 1) / cell;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t col = 0; col < cols; ++col) {
                const uint32_t x = region_x + col * cell;
                const uint32_t y = region_y + row * cell;
                if (x >= width || y >= height) {
                    continue;
                }
                const uint32_t w = std::min(cell, width - x);
                const uint32_t h = std::min(cell, height - y);
                layout.rects.push_back({x, y, w, h, kChecker[(row + col) & 1u]});
            }
        }
    }

    // Diagonal staircase of blocks in the lower-left, drawn last.
    constexpr uint32_t kStairCount = 6;
    for (uint32_t i = 0; i < kStairCount; ++i) {
        const uint32_t x = static_cast<uint32_t>((static_cast<uint64_t>(width) * i) / 18);
        const uint32_t y =
            height -
            static_cast<uint32_t>((static_cast<uint64_t>(height) * (i + 1)) / (kStairCount + 4));
        if (x >= width || y >= height) {
            continue;
        }
        uint32_t w = std::max<uint32_t>(1, width / 10);
        uint32_t h = std::max<uint32_t>(1, height / 10);
        w = std::min(w, width - x);
        h = std::min(h, height - y);
        layout.rects.push_back({x, y, w, h, kStairs[i]});
    }

    return layout;
}

void paint_layout(const ScreenshotLayout &layout, uint8_t *rgba)
{
    if (layout.width == 0 || layout.height == 0 || rgba == nullptr) {
        return;
    }
    const size_t pixels = static_cast<size_t>(layout.width) * layout.height;
    for (size_t i = 0; i < pixels; ++i) {
        store_rgba(rgba + i * 4, layout.background_rgba);
    }

    for (const ColorRect &rect : layout.rects) {
        if (rect.width == 0 || rect.height == 0) {
            continue;
        }
        if (rect.x >= layout.width || rect.y >= layout.height) {
            continue;
        }
        const uint32_t w = std::min(rect.width, layout.width - rect.x);
        const uint32_t h = std::min(rect.height, layout.height - rect.y);
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t *row = rgba + (static_cast<size_t>(rect.y + y) * layout.width + rect.x) * 4;
            for (uint32_t x = 0; x < w; ++x) {
                store_rgba(row + static_cast<size_t>(x) * 4, rect.rgba);
            }
        }
    }
}

std::vector<uint8_t> render_layout(const ScreenshotLayout &layout)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(layout.width) * layout.height * 4, 0);
    paint_layout(layout, rgba.data());
    return rgba;
}

size_t count_unique_colours(const uint8_t *rgba, size_t pixel_count)
{
    if (rgba == nullptr) {
        return 0;
    }
    std::vector<uint32_t> colours(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
        colours[i] = (static_cast<uint32_t>(rgba[i * 4]) << 24) |
                     (static_cast<uint32_t>(rgba[i * 4 + 1]) << 16) |
                     (static_cast<uint32_t>(rgba[i * 4 + 2]) << 8) |
                     static_cast<uint32_t>(rgba[i * 4 + 3]);
    }
    std::sort(colours.begin(), colours.end());
    colours.erase(std::unique(colours.begin(), colours.end()), colours.end());
    return colours.size();
}

} // namespace vkbridge
