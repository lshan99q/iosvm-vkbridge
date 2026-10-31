// SPDX-License-Identifier: MIT

#include "vkbridge/png.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace vkbridge
{
namespace
{

constexpr size_t kSignatureSize = 8;
constexpr size_t kChunkOverhead = 12; // length + type + CRC
constexpr uint8_t kSignature[kSignatureSize] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

uint32_t be32(const uint8_t *bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
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

uint32_t adler32_bytes(const uint8_t *data, size_t size)
{
    constexpr uint32_t kMod = 65521;
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t index = 0; index < size; ++index) {
        a = (a + data[index]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

Status decode_failure(const std::string &detail)
{
    return status_fail(Error::InvalidArgument, "PNG decode: " + detail);
}

} // namespace

Status decode_png(const uint8_t *bytes, size_t size, PngPixels &out)
{
    out = PngPixels{};
    if (bytes == nullptr) {
        return status_fail(Error::InvalidArgument, "PNG buffer is null");
    }
    if (size < kSignatureSize || std::memcmp(bytes, kSignature, kSignatureSize) != 0) {
        return decode_failure("missing PNG signature");
    }

    uint32_t width = 0;
    uint32_t height = 0;
    bool saw_ihdr = false;
    bool saw_iend = false;
    std::vector<uint8_t> idat;

    size_t position = kSignatureSize;
    while (position < size) {
        if (size - position < kChunkOverhead) {
            return decode_failure("truncated chunk header");
        }
        const uint32_t length = be32(bytes + position);
        if (length > size - position - kChunkOverhead) {
            return decode_failure("a chunk runs past the end of the file");
        }
        const uint8_t *type = bytes + position + 4;
        const uint8_t *data = bytes + position + 8;
        const uint32_t stored_crc = be32(data + length);
        const uint32_t computed_crc = crc32_bytes(type, 4 + static_cast<size_t>(length));
        if (stored_crc != computed_crc) {
            return decode_failure("chunk CRC mismatch");
        }

        const std::string name(reinterpret_cast<const char *>(type), 4);
        if (name == "IHDR") {
            if (saw_ihdr) {
                return decode_failure("duplicate IHDR");
            }
            if (length != 13) {
                return decode_failure("IHDR has the wrong length");
            }
            width = be32(data);
            height = be32(data + 4);
            if (data[8] != 8 || data[9] != 6 || data[10] != 0 || data[11] != 0 || data[12] != 0) {
                return decode_failure(
                    "unsupported IHDR (need 8-bit RGBA, deflate, filter 0, no interlacing)");
            }
            if (width == 0 || height == 0) {
                return decode_failure("IHDR has a zero dimension");
            }
            saw_ihdr = true;
        } else if (name == "IDAT") {
            if (!saw_ihdr) {
                return decode_failure("IDAT before IHDR");
            }
            idat.insert(idat.end(), data, data + length);
        } else if (name == "IEND") {
            if (length != 0) {
                return decode_failure("IEND has a payload");
            }
            saw_iend = true;
            position += kChunkOverhead;
            if (position != size) {
                return decode_failure("trailing bytes after IEND");
            }
            break;
        } else {
            return decode_failure("unexpected chunk \"" + name + "\"");
        }
        position += kChunkOverhead + length;
    }
    if (!saw_ihdr) {
        return decode_failure("no IHDR chunk");
    }
    if (!saw_iend) {
        return decode_failure("no IEND chunk");
    }
    if (idat.empty()) {
        return decode_failure("no IDAT chunk");
    }

    const uint64_t row_bytes = static_cast<uint64_t>(width) * 4u;
    const uint64_t raw_size = (row_bytes + 1u) * static_cast<uint64_t>(height);
    if (raw_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return decode_failure("the image does not fit in host memory");
    }
    if (raw_size > static_cast<uint64_t>(idat.size())) {
        return decode_failure("IHDR implies more pixels than the IDAT data can hold");
    }

    // zlib (RFC 1950) wrapping stored deflate blocks (RFC 1951).
    if (idat.size() < 6) {
        return decode_failure("zlib stream is too short");
    }
    if ((idat[0] & 0x0Fu) != 8) {
        return decode_failure("zlib compression method is not deflate");
    }
    if ((idat[0] >> 4) > 7) {
        return decode_failure("zlib window size is invalid");
    }
    if (((static_cast<uint32_t>(idat[0]) << 8) | idat[1]) % 31u != 0) {
        return decode_failure("zlib header check bits are wrong");
    }
    if ((idat[1] & 0x20u) != 0) {
        return decode_failure("zlib preset dictionaries are not supported");
    }

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(raw_size));
    size_t offset = 2;
    size_t blocks = 0;
    bool final_block = false;
    while (!final_block) {
        if (offset >= idat.size()) {
            return decode_failure("truncated deflate block header");
        }
        const uint8_t header = idat[offset++];
        final_block = (header & 0x01u) != 0;
        if (((header >> 1) & 0x03u) != 0) {
            return decode_failure("only stored deflate blocks are supported");
        }
        if (idat.size() - offset < 4) {
            return decode_failure("truncated stored block header");
        }
        const uint16_t length = static_cast<uint16_t>(idat[offset] | (idat[offset + 1] << 8));
        const uint16_t complement =
            static_cast<uint16_t>(idat[offset + 2] | (idat[offset + 3] << 8));
        offset += 4;
        if (static_cast<uint16_t>(~length) != complement) {
            return decode_failure("stored block length and complement disagree");
        }
        if (idat.size() - offset < length) {
            return decode_failure("truncated stored block payload");
        }
        raw.insert(raw.end(), idat.begin() + static_cast<std::ptrdiff_t>(offset),
                   idat.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
        ++blocks;
    }

    if (raw.size() != raw_size) {
        return decode_failure("decompressed size does not match IHDR");
    }
    if (idat.size() - offset != 4) {
        return decode_failure("expected exactly one Adler-32 after the final block");
    }
    if (be32(idat.data() + offset) != adler32_bytes(raw.data(), raw.size())) {
        return decode_failure("Adler-32 mismatch");
    }

    out.width = width;
    out.height = height;
    out.stored_blocks = blocks;
    out.rgba.resize(static_cast<size_t>(row_bytes) * height);
    for (uint32_t y = 0; y < height; ++y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(row_bytes + 1u);
        if (raw[row] != 0) {
            return decode_failure("only filter type 0 rows are supported");
        }
        std::copy(raw.begin() + static_cast<std::ptrdiff_t>(row + 1),
                  raw.begin() + static_cast<std::ptrdiff_t>(row + 1 + row_bytes),
                  out.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) *
                                                                 static_cast<size_t>(row_bytes)));
    }
    return status_ok();
}

Status read_png(const std::string &path, PngPixels &out)
{
    out = PngPixels{};
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return status_fail(Error::IoError, path + ": " + std::strerror(errno));
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return status_fail(Error::IoError, path + ": cannot seek");
    }
    const long length = std::ftell(file);
    if (length <= 0) {
        std::fclose(file);
        return status_fail(Error::IoError, path + ": empty or unseekable file");
    }
    std::rewind(file);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        return status_fail(Error::IoError, path + ": short read");
    }
    return decode_png(bytes.data(), bytes.size(), out);
}

} // namespace vkbridge
