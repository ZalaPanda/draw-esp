#include "image/crc32.h"

#include <cstdint>

namespace {

constexpr uint32_t legacy_crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

constexpr bool matches_legacy(const uint8_t *bytes, size_t size)
{
    return image::crc32(bytes, size) == legacy_crc32(bytes, size);
}

struct ByteSequence {
    uint8_t values[256];
    constexpr ByteSequence() : values{}
    {
        for (int i = 0; i < 256; ++i) values[i] = static_cast<uint8_t>(i);
    }
};
constexpr ByteSequence kAllBytes{};

struct PseudoRandomBytes {
    uint8_t values[1024];
    constexpr PseudoRandomBytes() : values{}
    {
        uint32_t state = 1;
        for (int i = 0; i < 1024; ++i) {
            state = state * 1664525u + 1013904223u;
            values[i] = static_cast<uint8_t>(state >> 24);
        }
    }
};
constexpr PseudoRandomBytes kPseudoRandom{};

constexpr uint8_t kCheck[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
constexpr uint8_t kBinary[] = {0x00, 0xFF, 0x80, 0x7F, 0x01, 0x02, 0x03, 0x04};

} // namespace

// Standard CRC-32 check value.
static_assert(image::crc32(kCheck, sizeof(kCheck)) == 0xCBF43926u);
static_assert(image::crc32(kBinary, 0) == 0u);

// The table implementation must remain bit-identical to the stored format.
static_assert(matches_legacy(kCheck, sizeof(kCheck)));
static_assert(matches_legacy(kBinary, sizeof(kBinary)));
static_assert(matches_legacy(kAllBytes.values, sizeof(kAllBytes.values)));
static_assert(matches_legacy(kPseudoRandom.values, sizeof(kPseudoRandom.values)));
