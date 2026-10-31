// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vkbridge
{
// Value written by the vkCmdFillBuffer check. The same constant is used by the
// probe, by the unit tests and by the README so the expected data is unambiguous.
constexpr uint32_t kFillValue32 = 0x5A5AA5A5u;

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

uint32_t read_le32(const uint8_t *bytes);
void write_le32(uint8_t *bytes, uint32_t value);

// Deterministic pseudo-random upload pattern used to prove that host writes and
// GPU reads/transfers agree byte for byte.
uint32_t upload_word(uint64_t word_index);
std::vector<uint8_t> make_upload_pattern(size_t bytes);
bool verify_upload_pattern(const uint8_t *data, size_t bytes);

// Verifies that every complete little-endian 32-bit word equals value32. The
// caller must pass a byte count that is a multiple of four.
bool verify_fill(const uint8_t *data, size_t bytes, uint32_t value32);

// A colour rectangle used by the offscreen screenshot. The rectangle list is
// painted in order, which makes the CPU model below and the GPU render pass
// produce exactly the same image.
struct ColorRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rgba = 0;
};

struct ScreenshotLayout {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t background_rgba = 0;
    std::vector<ColorRect> rects;
};

// 320x200 yields 22 distinct colours: background + 8 bands + 5 stripes +
// 2 checker colours + 6 staircase colours.
ScreenshotLayout make_screenshot_layout(uint32_t width, uint32_t height);

// Paints the layout (background first, then every rect in order) into a tightly
// packed RGBA8 buffer of width*height*4 bytes. This is the CPU reference the GPU
// output is compared against.
void paint_layout(const ScreenshotLayout &layout, uint8_t *rgba);
std::vector<uint8_t> render_layout(const ScreenshotLayout &layout);

size_t count_unique_colours(const uint8_t *rgba, size_t pixel_count);

} // namespace vkbridge
