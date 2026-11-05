#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace image {
struct DecodedImage {
    uint16_t *pixels = nullptr;
    int width = 0;
    int height = 0;
};

esp_err_t decode_jpeg(const uint8_t *data, size_t size, DecodedImage &image);
void release(DecodedImage &image);
}
