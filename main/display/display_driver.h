#pragma once

#include "esp_err.h"

namespace display {
esp_err_t initialize();
esp_err_t show_test_pattern();
esp_err_t clear();
esp_err_t decrease_brightness();
}
