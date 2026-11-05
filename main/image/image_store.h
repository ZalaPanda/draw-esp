#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "display/display_driver.h"

namespace image_store {
esp_err_t initialize();
esp_err_t save(const uint8_t *data, size_t size, display::FitMode fit);
esp_err_t load(uint8_t **data, size_t *size, display::FitMode *fit);
esp_err_t clear();
}
