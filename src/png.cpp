// SPDX-License-Identifier: MIT

#include "vkbridge/png.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace vkbridge
{
namespace
{

constexpr size_t kStoredBlockMax = 65535;
constexpr uint64_t kMaxDimension = 0x7FFFFFFFULL;

struct Crc32Table {
    uint32_t entries[256] = {};

    Crc32Table()
    {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
            }
            entries[i] = crc;
        }
    }
};

const Crc32Table &crc32_table()
{
    static const Crc32Table table;
    return table;
}

uint32_t crc32_begin()
{
    return 0xFFFFFFFFu;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    const Crc32Table &table = crc32_table();
    for (size_t i = 0; i < size; ++i) {
        crc = table.entries[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32_end(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

void append_be32(std::vector<uint8_t> &out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void append_chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    append_be32(out, static_cast<uint32_t>(data.size()));
    uint32_t crc = crc32_begin();
    crc = crc32_update(crc, reinterpret_cast<const uint8_t *>(type), 4);
    for (const uint8_t byte : data) {
        crc = crc32_update(crc, &byte, 1);
    }
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    append_be32(out, crc32_end(crc));
}

uint32_t adler32(const uint8_t *data, size_t size)
{
    constexpr uint32_t kMod = 65521;
    constexpr size_t kMaxChunk = 5552;
    uint32_t a = 1;
    uint32_t b = 0;
    size_t offset = 0;
    while (offset < size) {
        const size_t chunk = std::min(kMaxChunk, size - offset);
        for (size_t i = 0; i < chunk; ++i) {
            a += data[offset + i];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        offset += chunk;
    }
    return (b << 16) | a;
}

} // namespace

Status encode_png(std::vector<uint8_t> &out, const PngImage &image)
{
    out.clear();
    if (image.rgba == nullptr) {
        return status_fail(Error::InvalidArgument, "PNG pixel buffer is null");
    }
    if (image.width == 0 || image.height == 0) {
        return status_fail(Error::InvalidArgument, "PNG dimensions must be non-zero");
    }
    if (image.width > kMaxDimension || image.height > kMaxDimension) {
        return status_fail(Error::SizeTooLarge, "PNG dimensions exceed the format limit");
    }

    const uint64_t row_bytes = static_cast<uint64_t>(image.width) * 4;
    const uint64_t stride =
        image.stride_bytes == 0 ? row_bytes : static_cast<uint64_t>(image.stride_bytes);
    if (stride < row_bytes) {
        return status_fail(Error::InvalidArgument, "PNG stride is smaller than a row");
    }
    const uint64_t raw_size = (row_bytes + 1) * static_cast<uint64_t>(image.height);
    if (raw_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return status_fail(Error::SizeTooLarge, "PNG image is too large to encode");
    }

    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), kSignature, kSignature + 8);

    std::vector<uint8_t> ihdr;
    ihdr.reserve(13);
    append_be32(ihdr, image.width);
    append_be32(ihdr, image.height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // colour type: truecolour with alpha
    ihdr.push_back(0); // compression: deflate
    ihdr.push_back(0); // filter method: adaptive
    ihdr.push_back(0); // interlace: none
    append_chunk(out, "IHDR", ihdr);

    // Uncompressed scanlines: filter byte 0, then one row of RGBA pixels.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(raw_size));
    for (uint32_t y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const uint8_t *row = image.rgba + static_cast<size_t>(y) * static_cast<size_t>(stride);
        raw.insert(raw.end(), row, row + static_cast<size_t>(row_bytes));
    }

    // zlib stream (RFC 1950) wrapping stored deflate blocks (RFC 1951).
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78); // CMF: deflate, 32 KiB window
    zlib.push_back(0x01); // FLG: no dictionary, fastest; 0x7801 % 31 == 0
    size_t offset = 0;
    while (offset < raw.size()) {
        const size_t length = std::min(kStoredBlockMax, raw.size() - offset);
        const bool final_block = offset + length == raw.size();
        zlib.push_back(final_block ? 0x01 : 0x00);
        const uint16_t len16 = static_cast<uint16_t>(length);
        const uint16_t nlen16 = static_cast<uint16_t>(~len16);
        zlib.push_back(static_cast<uint8_t>(len16 & 0xFFu));
        zlib.push_back(static_cast<uint8_t>((len16 >> 8) & 0xFFu));
        zlib.push_back(static_cast<uint8_t>(nlen16 & 0xFFu));
        zlib.push_back(static_cast<uint8_t>((nlen16 >> 8) & 0xFFu));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    append_be32(zlib, adler32(raw.data(), raw.size()));
    append_chunk(out, "IDAT", zlib);
    append_chunk(out, "IEND", {});

    return status_ok();
}

Status write_png(const std::string &path, const PngImage &image)
{
    std::vector<uint8_t> bytes;
    const Status encoded = encode_png(bytes, image);
    if (!encoded) {
        return encoded;
    }
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return status_fail(Error::IoError, path + ": " + std::strerror(errno));
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    const int closed = std::fclose(file);
    if (written != bytes.size() || closed != 0) {
        return status_fail(Error::IoError, path + ": short write");
    }
    return status_ok();
}

} // namespace vkbridge
