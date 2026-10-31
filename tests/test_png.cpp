// SPDX-License-Identifier: MIT

#include "test_framework.hpp"
#include "vkbridge/png.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

using vkbridge::encode_png;
using vkbridge::Error;
using vkbridge::PngImage;
using vkbridge::write_png;

namespace
{

uint32_t be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint32_t crc32_bytes(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t adler32_bytes(const uint8_t *data, size_t size)
{
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

struct DecodedZlib {
    std::vector<uint8_t> raw;
    size_t blocks = 0;
};

DecodedZlib decode_stored_zlib(const std::vector<uint8_t> &zlib)
{
    VKB_CHECK(zlib.size() >= 6);
    VKB_CHECK_EQ(static_cast<int>(zlib[0]), 0x78);
    VKB_CHECK_EQ(((static_cast<int>(zlib[0]) << 8) | static_cast<int>(zlib[1])) % 31, 0);

    DecodedZlib decoded;
    size_t position = 2;
    while (true) {
        VKB_CHECK(position < zlib.size());
        const uint8_t header = zlib[position++];
        const bool final_block = (header & 0x01u) != 0;
        const int block_type = (header >> 1) & 0x03;
        VKB_CHECK_EQ(block_type, 0);

        VKB_CHECK(position + 4 <= zlib.size());
        const uint16_t length = static_cast<uint16_t>(zlib[position] | (zlib[position + 1] << 8));
        const uint16_t complement =
            static_cast<uint16_t>(zlib[position + 2] | (zlib[position + 3] << 8));
        VKB_CHECK_EQ(static_cast<int>(complement),
                     static_cast<int>(static_cast<uint16_t>(~length)));
        position += 4;

        VKB_CHECK(position + length <= zlib.size());
        decoded.raw.insert(decoded.raw.end(), zlib.begin() + position,
                           zlib.begin() + position + length);
        position += length;
        ++decoded.blocks;
        if (final_block) {
            break;
        }
    }

    VKB_CHECK_EQ(position + 4, zlib.size());
    VKB_CHECK_EQ(be32(&zlib[position]), adler32_bytes(decoded.raw.data(), decoded.raw.size()));
    return decoded;
}

struct DecodedPng {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t colour_type = 0;
    uint8_t compression = 0;
    uint8_t filter_method = 0;
    uint8_t interlace = 0;
    size_t stored_blocks = 0;
    size_t idat_bytes = 0;
    std::vector<uint8_t> pixels;
};

DecodedPng decode_png(const std::vector<uint8_t> &png)
{
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    VKB_CHECK(png.size() >= 8);
    VKB_CHECK(std::equal(signature, signature + 8, png.begin()));

    DecodedPng decoded;
    std::vector<uint8_t> idat;
    bool saw_ihdr = false;
    bool saw_iend = false;
    size_t position = 8;
    while (position + 12 <= png.size()) {
        const uint32_t length = be32(&png[position]);
        VKB_CHECK(position + 12 + length <= png.size());
        const uint8_t *type = &png[position + 4];
        const uint8_t *data = &png[position + 8];
        VKB_CHECK_EQ(be32(&png[position + 8 + length]), crc32_bytes(type, 4 + length));

        const std::string name(reinterpret_cast<const char *>(type), 4);
        if (name == "IHDR") {
            VKB_CHECK_EQ(length, 13u);
            decoded.width = be32(data);
            decoded.height = be32(data + 4);
            decoded.bit_depth = data[8];
            decoded.colour_type = data[9];
            decoded.compression = data[10];
            decoded.filter_method = data[11];
            decoded.interlace = data[12];
            saw_ihdr = true;
        } else if (name == "IDAT") {
            decoded.idat_bytes += length;
            idat.insert(idat.end(), data, data + length);
        } else if (name == "IEND") {
            VKB_CHECK_EQ(length, 0u);
            saw_iend = true;
            break;
        }
        position += 12 + length;
    }
    VKB_CHECK(saw_ihdr);
    VKB_CHECK(saw_iend);
    VKB_CHECK(!idat.empty());

    const DecodedZlib zlib = decode_stored_zlib(idat);
    decoded.stored_blocks = zlib.blocks;

    const size_t row_bytes = static_cast<size_t>(decoded.width) * 4;
    VKB_CHECK_EQ(zlib.raw.size(), (row_bytes + 1) * decoded.height);
    decoded.pixels.resize(row_bytes * decoded.height);
    for (uint32_t y = 0; y < decoded.height; ++y) {
        const size_t row = static_cast<size_t>(y) * (row_bytes + 1);
        VKB_CHECK_EQ(static_cast<int>(zlib.raw[row]), 0);
        std::copy(zlib.raw.begin() + static_cast<std::ptrdiff_t>(row + 1),
                  zlib.raw.begin() + static_cast<std::ptrdiff_t>(row + 1 + row_bytes),
                  decoded.pixels.begin() + static_cast<std::ptrdiff_t>(y * row_bytes));
    }
    return decoded;
}

std::vector<uint8_t> make_gradient(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            pixels[index + 0] = static_cast<uint8_t>((x * 17 + y * 3) & 0xFF);
            pixels[index + 1] = static_cast<uint8_t>((y * 29 + x) & 0xFF);
            pixels[index + 2] = static_cast<uint8_t>((x * y + 7) & 0xFF);
            pixels[index + 3] = 255;
        }
    }
    return pixels;
}

} // namespace

VKB_TEST(png_encodes_single_pixel)
{
    const uint8_t pixel[4] = {255, 0, 0, 255};
    PngImage image;
    image.rgba = pixel;
    image.width = 1;
    image.height = 1;

    std::vector<uint8_t> bytes;
    const auto status = encode_png(bytes, image);
    VKB_CHECK(status.ok());

    const DecodedPng decoded = decode_png(bytes);
    VKB_CHECK_EQ(decoded.width, 1u);
    VKB_CHECK_EQ(decoded.height, 1u);
    VKB_CHECK_EQ(static_cast<int>(decoded.bit_depth), 8);
    VKB_CHECK_EQ(static_cast<int>(decoded.colour_type), 6);
    VKB_CHECK_EQ(static_cast<int>(decoded.compression), 0);
    VKB_CHECK_EQ(static_cast<int>(decoded.filter_method), 0);
    VKB_CHECK_EQ(static_cast<int>(decoded.interlace), 0);
    VKB_CHECK_EQ(decoded.stored_blocks, static_cast<size_t>(1));
    VKB_CHECK(decoded.pixels == std::vector<uint8_t>(pixel, pixel + 4));
}

VKB_TEST(png_respects_strided_rows)
{
    const uint32_t width = 5;
    const uint32_t height = 3;
    const uint32_t stride = width * 4 + 8;
    std::vector<uint8_t> canvas(static_cast<size_t>(stride) * height, 0xEE);
    std::vector<uint8_t> expected(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            expected[index + 0] = static_cast<uint8_t>(x * 40);
            expected[index + 1] = static_cast<uint8_t>(y * 70);
            expected[index + 2] = static_cast<uint8_t>(index & 0xFF);
            expected[index + 3] = 255;
            std::copy(expected.begin() + static_cast<std::ptrdiff_t>(index),
                      expected.begin() + static_cast<std::ptrdiff_t>(index + 4),
                      canvas.begin() + static_cast<std::ptrdiff_t>(y * stride + x * 4));
        }
    }

    PngImage image;
    image.rgba = canvas.data();
    image.width = width;
    image.height = height;
    image.stride_bytes = stride;

    std::vector<uint8_t> bytes;
    VKB_CHECK(encode_png(bytes, image).ok());
    const DecodedPng decoded = decode_png(bytes);
    VKB_CHECK_EQ(decoded.width, width);
    VKB_CHECK_EQ(decoded.height, height);
    VKB_CHECK(decoded.pixels == expected);
}

VKB_TEST(png_encodes_multi_block_image)
{
    const uint32_t width = 320;
    const uint32_t height = 200;
    const std::vector<uint8_t> pixels = make_gradient(width, height);

    PngImage image;
    image.rgba = pixels.data();
    image.width = width;
    image.height = height;

    std::vector<uint8_t> bytes;
    VKB_CHECK(encode_png(bytes, image).ok());
    const DecodedPng decoded = decode_png(bytes);
    VKB_CHECK(decoded.stored_blocks > 1);
    VKB_CHECK(decoded.pixels == pixels);
}

VKB_TEST(png_rejects_invalid_images)
{
    std::vector<uint8_t> bytes;
    uint8_t pixel[16] = {};

    PngImage image;
    image.width = 4;
    image.height = 4;
    VKB_CHECK(encode_png(bytes, image).code() == Error::InvalidArgument);

    image.rgba = pixel;
    image.width = 0;
    image.height = 4;
    VKB_CHECK(encode_png(bytes, image).code() == Error::InvalidArgument);

    image.width = 4;
    image.height = 0;
    VKB_CHECK(encode_png(bytes, image).code() == Error::InvalidArgument);

    image.width = 4;
    image.height = 4;
    image.stride_bytes = 8;
    VKB_CHECK(encode_png(bytes, image).code() == Error::InvalidArgument);
}

VKB_TEST(png_write_file_roundtrip)
{
    const uint32_t width = 13;
    const uint32_t height = 7;
    const std::vector<uint8_t> pixels = make_gradient(width, height);

    PngImage image;
    image.rgba = pixels.data();
    image.width = width;
    image.height = height;

    const std::string path =
        "/tmp/vkbridge-png-test-" + std::to_string(static_cast<long>(getpid())) + ".png";
    VKB_CHECK(write_png(path, image).ok());

    std::FILE *file = std::fopen(path.c_str(), "rb");
    VKB_CHECK(file != nullptr);
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    VKB_CHECK(size > 0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    std::remove(path.c_str());
    VKB_CHECK_EQ(read, bytes.size());

    const DecodedPng decoded = decode_png(bytes);
    VKB_CHECK(decoded.pixels == pixels);

    std::vector<uint8_t> corrupted = bytes;
    corrupted[20] ^= 0x01;
    bool threw = false;
    try {
        decode_png(corrupted);
    } catch (const vkbtest::Failure &) {
        threw = true;
    }
    VKB_CHECK(threw);
}
