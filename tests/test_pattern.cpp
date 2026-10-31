// SPDX-License-Identifier: MIT

#include "test_framework.hpp"
#include "vkbridge/pattern.hpp"

#include <cstdint>
#include <vector>

using namespace vkbridge;

VKB_TEST(pattern_upload_words_are_deterministic)
{
    VKB_CHECK(upload_word(0) == upload_word(0));
    VKB_CHECK(upload_word(0) != upload_word(1));
    VKB_CHECK(upload_word(12345) == upload_word(12345));
    VKB_CHECK(upload_word(1) != upload_word(2));
}

VKB_TEST(pattern_roundtrip_and_corruption_detection)
{
    const auto pattern = make_upload_pattern(1u << 20);
    VKB_CHECK_EQ(pattern.size(), static_cast<size_t>(1u << 20));
    VKB_CHECK(verify_upload_pattern(pattern.data(), pattern.size()));

    auto corrupted = pattern;
    corrupted[12345] ^= 0x01;
    VKB_CHECK(!verify_upload_pattern(corrupted.data(), corrupted.size()));

    const auto odd = make_upload_pattern(10);
    VKB_CHECK_EQ(odd.size(), static_cast<size_t>(10));
    VKB_CHECK(verify_upload_pattern(odd.data(), odd.size()));
}

VKB_TEST(pattern_little_endian_helpers)
{
    uint8_t bytes[4] = {};
    write_le32(bytes, 0x01020304u);
    VKB_CHECK_EQ(bytes[0], static_cast<uint8_t>(0x04));
    VKB_CHECK_EQ(bytes[1], static_cast<uint8_t>(0x03));
    VKB_CHECK_EQ(bytes[2], static_cast<uint8_t>(0x02));
    VKB_CHECK_EQ(bytes[3], static_cast<uint8_t>(0x01));
    VKB_CHECK_EQ(read_le32(bytes), 0x01020304u);

    write_le32(bytes, 0xFFFFFFFFu);
    VKB_CHECK_EQ(read_le32(bytes), 0xFFFFFFFFu);
}

VKB_TEST(pattern_fill_verification)
{
    std::vector<uint8_t> words(64 * 4);
    for (size_t index = 0; index < 64; ++index) {
        write_le32(words.data() + index * 4, kFillValue32);
    }
    VKB_CHECK(verify_fill(words.data(), words.size(), kFillValue32));
    VKB_CHECK(!verify_fill(words.data(), words.size(), kFillValue32 ^ 1u));
    VKB_CHECK(!verify_fill(words.data(), words.size() - 2, kFillValue32));

    words[7 * 4 + 3] = 0x00;
    VKB_CHECK(!verify_fill(words.data(), words.size(), kFillValue32));
}

VKB_TEST(pattern_layout_default_frame)
{
    const ScreenshotLayout layout = make_screenshot_layout(320, 200);
    VKB_CHECK_EQ(layout.width, 320u);
    VKB_CHECK_EQ(layout.height, 200u);
    VKB_CHECK(!layout.rects.empty());

    for (const ColorRect &rect : layout.rects) {
        VKB_CHECK(rect.width > 0);
        VKB_CHECK(rect.height > 0);
        VKB_CHECK(rect.x < layout.width);
        VKB_CHECK(rect.y < layout.height);
        VKB_CHECK(static_cast<uint64_t>(rect.x) + rect.width <= layout.width);
        VKB_CHECK(static_cast<uint64_t>(rect.y) + rect.height <= layout.height);
    }

    const std::vector<uint8_t> pixels = render_layout(layout);
    VKB_CHECK_EQ(pixels.size(), static_cast<size_t>(320u * 200u * 4u));
    VKB_CHECK_EQ(count_unique_colours(pixels.data(), 320u * 200u), static_cast<size_t>(22));

    // Painting is deterministic; the probe compares the GPU frame against it.
    VKB_CHECK(render_layout(layout) == pixels);
}

VKB_TEST(pattern_layout_tiny_frames_stay_in_bounds)
{
    const uint32_t sizes[][2] = {{1, 1}, {2, 3}, {17, 5}, {64, 48}, {320, 1}};
    for (const auto &size : sizes) {
        const ScreenshotLayout layout = make_screenshot_layout(size[0], size[1]);
        VKB_CHECK_EQ(layout.width, size[0]);
        VKB_CHECK_EQ(layout.height, size[1]);
        for (const ColorRect &rect : layout.rects) {
            VKB_CHECK(rect.width > 0);
            VKB_CHECK(rect.height > 0);
            VKB_CHECK(static_cast<uint64_t>(rect.x) + rect.width <= layout.width);
            VKB_CHECK(static_cast<uint64_t>(rect.y) + rect.height <= layout.height);
        }
        const std::vector<uint8_t> pixels = render_layout(layout);
        VKB_CHECK_EQ(pixels.size(), static_cast<size_t>(size[0]) * size[1] * 4);
    }
}

VKB_TEST(pattern_paint_clips_out_of_bounds_rects)
{
    ScreenshotLayout layout;
    layout.width = 2;
    layout.height = 2;
    layout.background_rgba = pack_rgba(0, 0, 0, 255);
    layout.rects.push_back({1, 1, 5, 5, pack_rgba(255, 0, 0, 255)});

    const std::vector<uint8_t> pixels = render_layout(layout);
    VKB_CHECK_EQ(pixels.size(), static_cast<size_t>(16));
    // (0,0) keeps the background, (1,1) is inside the clipped rectangle.
    VKB_CHECK_EQ(pixels[0], static_cast<uint8_t>(0));
    VKB_CHECK_EQ(pixels[15], static_cast<uint8_t>(255));
    VKB_CHECK_EQ(pixels[15 - 2], static_cast<uint8_t>(0));
    VKB_CHECK_EQ(count_unique_colours(pixels.data(), 4), static_cast<size_t>(2));
}
