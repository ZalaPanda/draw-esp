#pragma once

#include "esp_err.h"

namespace power_management {
esp_err_t initialize();
esp_err_t start_power_key_monitor(void (*on_short_press)());
}
