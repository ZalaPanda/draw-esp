#pragma once

#include "esp_err.h"

namespace input {
esp_err_t start_orientation_monitor();
// Returns the latest QMI8658 temperature reading. Returns false until the first
// successful read; the cached value stays available if a later read fails.
bool orientation_temperature_celsius(float &celsius);
} // namespace input
