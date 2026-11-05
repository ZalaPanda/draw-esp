#include "image_decoder.h"

#include <cstring>
#include "esp_heap_caps.h"
#include "png.h"

namespace {
constexpr uint32_t kMaximumDimension = 480;
}

namespace image {
esp_err_t decode_png(const uint8_t *data, size_t size, DecodedImage &image)
{
    image = {};
    if (data == nullptr || size < 8 || png_sig_cmp(data, 0, 8) != 0) return ESP_ERR_INVALID_ARG;

    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, data, size)) return ESP_ERR_INVALID_RESPONSE;
    if (png.width == 0 || png.height == 0 || png.width > kMaximumDimension ||
        png.height > kMaximumDimension) {
        png_image_free(&png);
        return ESP_ERR_INVALID_SIZE;
    }

    png.format = PNG_FORMAT_RGBA;
    const size_t pixel_count = static_cast<size_t>(png.width) * png.height;
    const size_t rgba_bytes = PNG_IMAGE_SIZE(png);
    auto *rgba = static_cast<uint8_t *>(heap_caps_malloc(rgba_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(pixel_count * sizeof(uint16_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rgba == nullptr || pixels == nullptr) {
        heap_caps_free(rgba);
        heap_caps_free(pixels);
        png_image_free(&png);
        return ESP_ERR_NO_MEM;
    }

    if (!png_image_finish_read(&png, nullptr, rgba, 0, nullptr)) {
        heap_caps_free(rgba);
        heap_caps_free(pixels);
        png_image_free(&png);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t width = png.width;
    const uint32_t height = png.height;
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint32_t alpha = rgba[4 * i + 3];
        const uint32_t red = (static_cast<uint32_t>(rgba[4 * i]) * alpha + 127) / 255;
        const uint32_t green = (static_cast<uint32_t>(rgba[4 * i + 1]) * alpha + 127) / 255;
        const uint32_t blue = (static_cast<uint32_t>(rgba[4 * i + 2]) * alpha + 127) / 255;
        pixels[i] = static_cast<uint16_t>(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
    }
    heap_caps_free(rgba);
    png_image_free(&png);
    image.pixels = pixels;
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    return ESP_OK;
}
}
