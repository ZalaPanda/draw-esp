#pragma once

#include "esp_err.h"

namespace display {
esp_err_t initialize_brightness_settings(unsigned &percent);
esp_err_t save_brightness(unsigned percent);
}
