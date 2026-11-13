#pragma once

#include "esp_err.h"
#include <cstddef>
#include <cstdint>

namespace telemetry {

struct Snapshot {
    uint64_t uptime_us = 0;
    bool cpu_temperature_valid = false;
    float cpu_temperature_c = 0.0f;
    bool imu_temperature_valid = false;
    float imu_temperature_c = 0.0f;
    bool cpu_load_valid = false;
    unsigned cpu_load_percent = 0;
    size_t internal_free_bytes = 0;
    size_t internal_minimum_free_bytes = 0;
    size_t internal_largest_free_block_bytes = 0;
    size_t psram_free_bytes = 0;
    size_t psram_largest_free_block_bytes = 0;
};

// Starts the sampling task and the ESP32-S3 internal temperature sensor.
// A sensor failure is logged and only disables that reading; the task still
// samples CPU load and heap. Returns an error only if the task cannot start.
esp_err_t initialize();

// Returns the most recently published sample. Safe to call from any task.
Snapshot snapshot();

} // namespace telemetry
