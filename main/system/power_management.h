#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

namespace power_management {
esp_err_t initialize();
 i2c_master_bus_handle_t i2c_bus();
esp_err_t start_power_key_monitor(void (*on_short_press)());
}
