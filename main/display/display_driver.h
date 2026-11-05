#pragma once

#include "esp_err.h"
#include <cstdint>

namespace display {
enum class FitMode { contain, cover, stretch };
esp_err_t initialize();
esp_err_t show_test_pattern();
esp_err_t show_rgb565(const uint16_t *pixels, int width, int height, FitMode fit);
esp_err_t clear();
esp_err_t decrease_brightness();
}
