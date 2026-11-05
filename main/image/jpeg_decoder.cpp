#include "image_decoder.h"

#include <algorithm>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_decoder.h"

namespace {
constexpr char kTag[] = "jpeg-decoder";
constexpr size_t kMaximumDecodedBytes = 480u * 480u * sizeof(uint16_t);
}

namespace image {
esp_err_t decode_jpeg(const uint8_t *data, size_t size, DecodedImage &image)
{
    image = {};
    if (data == nullptr || size < 4 || size > UINT32_MAX) return ESP_ERR_INVALID_ARG;

    esp_jpeg_image_cfg_t config = {};
    config.indata = const_cast<uint8_t *>(data);
    config.indata_size = static_cast<uint32_t>(size);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    config.out_scale = JPEG_IMAGE_SCALE_0;
    config.flags.swap_color_bytes = 0;
    esp_jpeg_image_output_t info = {};
    esp_err_t error = esp_jpeg_get_image_info(&config, &info);
    if (error != ESP_OK || info.width == 0 || info.height == 0) return ESP_ERR_INVALID_RESPONSE;

    constexpr esp_jpeg_image_scale_t scales[] = {JPEG_IMAGE_SCALE_0, JPEG_IMAGE_SCALE_1_2,
                                                  JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_8};
    esp_jpeg_image_scale_t selected = scales[3];
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    for (const auto scale : scales) {
        const unsigned divisor = 1u << static_cast<unsigned>(scale);
        output_width = std::max<uint32_t>(1, (static_cast<uint32_t>(info.width) + divisor - 1) / divisor);
        output_height = std::max<uint32_t>(1, (static_cast<uint32_t>(info.height) + divisor - 1) / divisor);
        if (output_width <= 480 && output_height <= 480) {
            selected = scale;
            break;
        }
    }
    const size_t capacity = static_cast<size_t>(output_width) * output_height * sizeof(uint16_t);
    if (capacity == 0 || capacity > kMaximumDecodedBytes) return ESP_ERR_INVALID_SIZE;
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) return ESP_ERR_NO_MEM;
    config.out_scale = selected;
    config.outbuf = reinterpret_cast<uint8_t *>(pixels);
    config.outbuf_size = static_cast<uint32_t>(capacity);
    error = esp_jpeg_decode(&config, &info);
    if (error != ESP_OK || info.width == 0 || info.height == 0 || info.output_len > capacity) {
        heap_caps_free(pixels);
        ESP_LOGW(kTag, "JPEG decode failed: %s", esp_err_to_name(error));
        return ESP_ERR_INVALID_RESPONSE;
    }
    image.pixels = pixels;
    image.width = info.width;
    image.height = info.height;
    return ESP_OK;
}

void release(DecodedImage &image)
{
    if (image.pixels != nullptr) heap_caps_free(image.pixels);
    image = {};
}
}
