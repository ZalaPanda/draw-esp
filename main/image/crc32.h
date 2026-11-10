#pragma once

#include <cstddef>
#include <cstdint>

namespace image {
namespace detail {

constexpr uint32_t crc32_entry(uint32_t index)
{
    uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

struct Crc32Table {
    uint32_t entries[256];
    constexpr Crc32Table() : entries{}
    {
        for (uint32_t i = 0; i < 256; ++i) entries[i] = crc32_entry(i);
    }
};
constexpr Crc32Table kCrc32Table{};

} // namespace detail

// Reflected IEEE CRC-32 (the same value the previous bit-at-a-time loop stored
// in image records). The table removes eight inner iterations per byte.
constexpr uint32_t crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ detail::kCrc32Table.entries[(crc ^ bytes[i]) & 0xFF];
    }
    return ~crc;
}

} // namespace image
