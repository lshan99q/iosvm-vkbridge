// SPDX-License-Identifier: MIT

#include "test_framework.hpp"
#include "vkbridge/png.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using vkbridge::decode_png;
using vkbridge::encode_png;
using vkbridge::Error;
using vkbridge::PngImage;
using vkbridge::PngPixels;
using vkbridge::read_png;
using vkbridge::write_png;

namespace
{

uint32_t be32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

void put_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    bytes[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    bytes[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    bytes[3] = static_cast<uint8_t>(value & 0xFFu);
}

uint32_t crc32_bytes(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
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

struct ChunkLocation {
    size_t data_offset = 0;
    uint32_t length = 0;
};

bool find_chunk(const std::vector<uint8_t> &png, const char *type, ChunkLocation &location)
{
    size_t position = 8;
    while (position + 12 <= png.size()) {
        const uint32_t length = be32(&png[position]);
        if (position + 12 + length > png.size()) {
            return false;
        }
        if (std::memcmp(&png[position + 4], type, 4) == 0) {
            location.data_offset = position + 8;
            location.length = length;
            return true;
        }
        position += 12 + length;
    }
    return false;
}

// Recomputes the CRC of the chunk that owns `data_offset` so a test can corrupt the chunk
// payload and still reach the checks that look inside it.
void rewrite_chunk_crc(std::vector<uint8_t> &png, size_t data_offset, uint32_t length)
{
    const size_t type_offset = data_offset - 4;
    const uint32_t crc = crc32_bytes(&png[type_offset], 4 + length);
    put_be32(&png[data_offset + length], crc);
}

std::vector<uint8_t> encode_gradient(uint32_t width, uint32_t height)
{
    const std::vector<uint8_t> pixels = make_gradient(width, height);
    PngImage image;
    image.rgba = pixels.data();
    image.width = width;
    image.height = height;
    std::vector<uint8_t> bytes;
    const auto status = encode_png(bytes, image);
    VKB_CHECK(status.ok());
    return bytes;
}

} // namespace

VKB_TEST(png_reader_decodes_writer_output)
{
    const uint32_t width = 5;
    const uint32_t height = 3;
    const std::vector<uint8_t> pixels = make_gradient(width, height);
    PngImage image;
    image.rgba = pixels.data();
    image.width = width;
    image.height = height;

    std::vector<uint8_t> bytes;
    VKB_CHECK(encode_png(bytes, image).ok());

    PngPixels decoded;
    VKB_CHECK(decode_png(bytes.data(), bytes.size(), decoded).ok());
    VKB_CHECK_EQ(decoded.width, width);
    VKB_CHECK_EQ(decoded.height, height);
    VKB_CHECK_EQ(decoded.stored_blocks, static_cast<size_t>(1));
    VKB_CHECK(decoded.rgba == pixels);
}

VKB_TEST(png_reader_decodes_multi_block_image)
{
    const uint32_t width = 320;
    const uint32_t height = 200;
    const std::vector<uint8_t> bytes = encode_gradient(width, height);
    PngPixels decoded;
    VKB_CHECK(decode_png(bytes.data(), bytes.size(), decoded).ok());
    VKB_CHECK_EQ(decoded.width, width);
    VKB_CHECK_EQ(decoded.height, height);
    VKB_CHECK(decoded.stored_blocks > 1);
    VKB_CHECK(decoded.rgba == make_gradient(width, height));
}

VKB_TEST(png_reader_rejects_bad_signature)
{
    std::vector<uint8_t> bytes = encode_gradient(3, 2);
    bytes[1] ^= 0x01;
    PngPixels decoded;
    const auto status = decode_png(bytes.data(), bytes.size(), decoded);
    VKB_CHECK(!status.ok());
    VKB_CHECK(status.code() == Error::InvalidArgument);
}

VKB_TEST(png_reader_rejects_bad_chunk_crc)
{
    std::vector<uint8_t> bytes = encode_gradient(3, 2);
    ChunkLocation idat;
    VKB_CHECK(find_chunk(bytes, "IDAT", idat));
    bytes[idat.data_offset + 1] ^= 0x01;
    PngPixels decoded;
    const auto status = decode_png(bytes.data(), bytes.size(), decoded);
    VKB_CHECK(!status.ok());
    VKB_CHECK(status.detail().find("CRC") != std::string::npos);
}

VKB_TEST(png_reader_rejects_bad_adler32)
{
    std::vector<uint8_t> bytes = encode_gradient(3, 2);
    ChunkLocation idat;
    VKB_CHECK(find_chunk(bytes, "IDAT", idat));
    // The zlib stream ends with the 4-byte Adler-32 of the raw scanlines.
    bytes[idat.data_offset + idat.length - 4] ^= 0x01;
    rewrite_chunk_crc(bytes, idat.data_offset, idat.length);
    PngPixels decoded;
    const auto status = decode_png(bytes.data(), bytes.size(), decoded);
    VKB_CHECK(!status.ok());
    VKB_CHECK(status.detail().find("Adler") != std::string::npos);
}

VKB_TEST(png_reader_rejects_unsupported_ihdr)
{
    std::vector<uint8_t> bytes = encode_gradient(3, 2);
    ChunkLocation ihdr;
    VKB_CHECK(find_chunk(bytes, "IHDR", ihdr));
    VKB_CHECK_EQ(ihdr.length, 13u);
    bytes[ihdr.data_offset + 9] = 2; // colour type RGB instead of RGBA
    rewrite_chunk_crc(bytes, ihdr.data_offset, ihdr.length);
    PngPixels decoded;
    const auto status = decode_png(bytes.data(), bytes.size(), decoded);
    VKB_CHECK(!status.ok());
    VKB_CHECK(status.detail().find("IHDR") != std::string::npos);
}

VKB_TEST(png_reader_rejects_truncated_file)
{
    std::vector<uint8_t> bytes = encode_gradient(3, 2);
    bytes.resize(bytes.size() - 20);
    PngPixels decoded;
    VKB_CHECK(!decode_png(bytes.data(), bytes.size(), decoded).ok());
}

VKB_TEST(png_reader_file_roundtrip)
{
    const uint32_t width = 13;
    const uint32_t height = 7;
    const std::vector<uint8_t> pixels = make_gradient(width, height);
    PngImage image;
    image.rgba = pixels.data();
    image.width = width;
    image.height = height;

    const std::string path =
        "/tmp/vkbridge-png-reader-test-" + std::to_string(static_cast<long>(getpid())) + ".png";
    VKB_CHECK(write_png(path, image).ok());
    PngPixels decoded;
    const auto status = read_png(path, decoded);
    std::remove(path.c_str());
    VKB_CHECK(status.ok());
    VKB_CHECK_EQ(decoded.width, width);
    VKB_CHECK_EQ(decoded.height, height);
    VKB_CHECK(decoded.rgba == pixels);
}
