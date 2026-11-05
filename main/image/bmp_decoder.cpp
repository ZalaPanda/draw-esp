#include "image_decoder.h"

#include <limits>
#include "esp_heap_caps.h"

namespace {
constexpr uint32_t kMaximumDimension = 480;

uint16_t read_u16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

uint32_t read_u32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}

namespace image {
esp_err_t decode_bmp(const uint8_t *data, size_t size, DecodedImage &image)
{
    image = {};
    if (data == nullptr || size < 54 || data[0] != 'B' || data[1] != 'M') return ESP_ERR_INVALID_ARG;

    const uint32_t pixel_offset = read_u32(data + 10);
    const uint32_t dib_size = read_u32(data + 14);
    if (dib_size < 40 || static_cast<uint64_t>(14) + dib_size > size || pixel_offset > size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int32_t signed_width = static_cast<int32_t>(read_u32(data + 18));
    const int32_t signed_height = static_cast<int32_t>(read_u32(data + 22));
    const uint16_t planes = read_u16(data + 26);
    const uint16_t bits_per_pixel = read_u16(data + 28);
    const uint32_t compression = read_u32(data + 30);
    if (signed_width <= 0 || signed_height == 0 || signed_height == INT32_MIN || planes != 1 ||
        (bits_per_pixel != 24 && bits_per_pixel != 32) || compression != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t width = static_cast<uint32_t>(signed_width);
    const uint32_t height = static_cast<uint32_t>(signed_height < 0 ? -signed_height : signed_height);
    if (width > kMaximumDimension || height > kMaximumDimension) return ESP_ERR_INVALID_SIZE;

    const uint64_t row_bytes = ((static_cast<uint64_t>(width) * bits_per_pixel + 31) / 32) * 4;
    const uint64_t pixel_bytes = row_bytes * height;
    if (row_bytes > std::numeric_limits<size_t>::max() || pixel_offset + pixel_bytes > size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(static_cast<size_t>(width) * height * sizeof(uint16_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) return ESP_ERR_NO_MEM;
    const size_t bytes_per_pixel = bits_per_pixel / 8;
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t source_y = signed_height > 0 ? height - 1 - y : y;
        const uint8_t *row = data + pixel_offset + static_cast<size_t>(source_y * row_bytes);
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *pixel = row + x * bytes_per_pixel;
            pixels[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(
                ((pixel[2] >> 3) << 11) | ((pixel[1] >> 2) << 5) | (pixel[0] >> 3));
        }
    }
    image.pixels = pixels;
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    return ESP_OK;
}
}
