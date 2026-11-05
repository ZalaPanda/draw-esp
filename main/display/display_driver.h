#pragma once

#include "esp_err.h"
#include <cstdint>

namespace display {
enum class FitMode { contain, cover, stretch };
enum class Rotation : uint8_t { deg0, deg90, deg180, deg270 };
esp_err_t initialize();
esp_err_t show_test_pattern();
esp_err_t show_rgb565(const uint16_t *pixels, int width, int height, FitMode fit);
esp_err_t set_rotation(Rotation rotation);
esp_err_t clear();
esp_err_t decrease_brightness();
}
