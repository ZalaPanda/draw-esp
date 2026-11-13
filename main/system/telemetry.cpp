#include "telemetry.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cpu_load.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input/orientation.h"

namespace {
constexpr char kTag[] = "telemetry";
constexpr uint32_t kSampleIntervalMs = 30000;
constexpr unsigned kLogEverySamples = 2; // 60 s at the 30 s sample interval
constexpr int kCpuTemperatureRangeMin = 20;
constexpr int kCpuTemperatureRangeMax = 100;
constexpr unsigned kCoreCount = CONFIG_FREERTOS_NUMBER_OF_CORES;
static_assert(kCoreCount > 0);

portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
telemetry::Snapshot published{};
temperature_sensor_handle_t cpu_sensor = nullptr;
bool task_started = false;

bool read_idle_counters(configRUN_TIME_COUNTER_TYPE *values)
{
    for (unsigned core = 0; core < kCoreCount; ++core) {
        const TaskHandle_t idle = xTaskGetIdleTaskHandleForCore(static_cast<BaseType_t>(core));
        if (idle == nullptr) return false;
        TaskStatus_t status = {};
        vTaskGetInfo(idle, &status, pdFALSE, eInvalid);
        values[core] = status.ulRunTimeCounter;
    }
    return true;
}

void publish(const telemetry::Snapshot &value)
{
    portENTER_CRITICAL(&snapshot_lock);
    published = value;
    portEXIT_CRITICAL(&snapshot_lock);
}

void collect_heap(telemetry::Snapshot &value)
{
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    value.internal_free_bytes = info.total_free_bytes;
    value.internal_minimum_free_bytes = info.minimum_free_bytes;
    value.internal_largest_free_block_bytes = info.largest_free_block;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    value.psram_free_bytes = info.total_free_bytes;
    value.psram_largest_free_block_bytes = info.largest_free_block;
}

void collect_readings(telemetry::Snapshot &value)
{
    value.uptime_us = static_cast<uint64_t>(esp_timer_get_time());

    float celsius = 0.0f;
    value.cpu_temperature_valid = cpu_sensor != nullptr &&
        temperature_sensor_get_celsius(cpu_sensor, &celsius) == ESP_OK && std::isfinite(celsius);
    value.cpu_temperature_c = celsius;

    float imu_celsius = 0.0f;
    value.imu_temperature_valid = input::orientation_temperature_celsius(imu_celsius) &&
        std::isfinite(imu_celsius);
    value.imu_temperature_c = imu_celsius;

    collect_heap(value);
}

void log_snapshot(const telemetry::Snapshot &value)
{
    char cpu_temperature[16] = "n/a";
    if (value.cpu_temperature_valid) {
        std::snprintf(cpu_temperature, sizeof(cpu_temperature), "%.1f C", value.cpu_temperature_c);
    }
    char imu_temperature[16] = "n/a";
    if (value.imu_temperature_valid) {
        std::snprintf(imu_temperature, sizeof(imu_temperature), "%.1f C", value.imu_temperature_c);
    }
    char cpu_load[16] = "n/a";
    if (value.cpu_load_valid) {
        std::snprintf(cpu_load, sizeof(cpu_load), "%u%%", value.cpu_load_percent);
    }
    ESP_LOGD(kTag,
             "SoC %s, IMU %s, CPU %s, internal free %u B (min %u B, largest %u B), "
             "PSRAM free %u B (largest %u B)",
             cpu_temperature, imu_temperature, cpu_load,
             static_cast<unsigned>(value.internal_free_bytes),
             static_cast<unsigned>(value.internal_minimum_free_bytes),
             static_cast<unsigned>(value.internal_largest_free_block_bytes),
             static_cast<unsigned>(value.psram_free_bytes),
             static_cast<unsigned>(value.psram_largest_free_block_bytes));
}

void telemetry_task(void *)
{
    configRUN_TIME_COUNTER_TYPE previous_idle[kCoreCount] = {};
    uint64_t previous_us = static_cast<uint64_t>(esp_timer_get_time());
    bool have_previous_idle = read_idle_counters(previous_idle);
    unsigned sample = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));

        telemetry::Snapshot next = {};
        collect_readings(next);

        configRUN_TIME_COUNTER_TYPE current_idle[kCoreCount] = {};
        if (read_idle_counters(current_idle)) {
            if (have_previous_idle) {
                uint64_t idle_delta_us = 0;
                for (unsigned core = 0; core < kCoreCount; ++core) {
                    idle_delta_us += telemetry::counter_delta(current_idle[core], previous_idle[core]);
                }
                const uint64_t elapsed_us =
                    next.uptime_us > previous_us ? next.uptime_us - previous_us : 0;
                if (elapsed_us > 0) {
                    next.cpu_load_valid = true;
                    next.cpu_load_percent =
                        telemetry::cpu_load_percent(idle_delta_us, elapsed_us, kCoreCount);
                }
            }
            std::memcpy(previous_idle, current_idle, sizeof(previous_idle));
            previous_us = next.uptime_us;
            have_previous_idle = true;
        }
        publish(next);

        if ((++sample % kLogEverySamples) == 0) log_snapshot(next);
    }
}
} // namespace

namespace telemetry {
esp_err_t initialize()
{
    if (task_started) return ESP_ERR_INVALID_STATE;

    // The driver logs its selected range at INFO on install; keep startup quiet
    // while preserving its warnings and errors.
    esp_log_level_set("temperature_sensor", ESP_LOG_WARN);
    temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(kCpuTemperatureRangeMin, kCpuTemperatureRangeMax);
    esp_err_t sensor_error = temperature_sensor_install(&config, &cpu_sensor);
    if (sensor_error == ESP_OK) {
        sensor_error = temperature_sensor_enable(cpu_sensor);
        if (sensor_error != ESP_OK) {
            temperature_sensor_uninstall(cpu_sensor);
            cpu_sensor = nullptr;
        }
    }
    if (cpu_sensor == nullptr) {
        ESP_LOGW(kTag, "ESP32-S3 temperature sensor unavailable: %s", esp_err_to_name(sensor_error));
    } else {
        ESP_LOGI(kTag, "ESP32-S3 temperature sensor ready (%d-%d C range)",
                 kCpuTemperatureRangeMin, kCpuTemperatureRangeMax);
    }

    // Publish heap and temperature immediately; CPU load becomes valid after
    // the first full 30 s idle-counter window.
    Snapshot initial = {};
    collect_readings(initial);
    publish(initial);

    ESP_RETURN_ON_FALSE(xTaskCreate(telemetry_task, "telemetry", 3072, nullptr, 2, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Telemetry task creation failed");
    task_started = true;
    ESP_LOGI(kTag, "System telemetry ready: 30 s sampling, CPU load and heap");
    return ESP_OK;
}

Snapshot snapshot()
{
    Snapshot copy = {};
    portENTER_CRITICAL(&snapshot_lock);
    copy = published;
    portEXIT_CRITICAL(&snapshot_lock);
    return copy;
}
} // namespace telemetry
