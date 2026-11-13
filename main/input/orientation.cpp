#include "orientation.h"

#include <cmath>
#include "display/display_driver.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef M_PI
#undef M_PI
#endif
#include "qmi8658.h"
#include "system/power_management.h"

namespace {
constexpr char kTag[] = "orientation";
constexpr float kGravity = 9.807f;
constexpr float kMinimumTiltAcceleration = 0.70f * kGravity;
constexpr unsigned kStableSamples = 8;
constexpr unsigned kTemperatureSampleDivider = 10; // about once per second

enum class Candidate : int { none = -1, deg0, deg90, deg180, deg270 };
qmi8658_dev_t sensor{};
bool task_started = false;
portMUX_TYPE temperature_lock = portMUX_INITIALIZER_UNLOCKED;
float cached_temperature_c = 0.0f;
bool cached_temperature_valid = false;

Candidate classify(float x, float y)
{
    // The board's display plane is XY. Ignore face-up/down and diagonal poses;
    // keeping the last confirmed orientation prevents boundary flicker.
    if (std::fabs(x) > std::fabs(y) && std::fabs(x) >= kMinimumTiltAcceleration) {
        return x > 0 ? Candidate::deg90 : Candidate::deg270;
    }
    if (std::fabs(y) >= kMinimumTiltAcceleration && std::fabs(y) >= std::fabs(x)) {
        return y > 0 ? Candidate::deg0 : Candidate::deg180;
    }
    return Candidate::none;
}

display::Rotation to_rotation(Candidate value)
{
    switch (value) {
    case Candidate::deg90: return display::Rotation::deg90;
    case Candidate::deg180: return display::Rotation::deg180;
    case Candidate::deg270: return display::Rotation::deg270;
    default: return display::Rotation::deg0;
    }
}

void orientation_task(void *)
{
    Candidate stable = Candidate::none;
    Candidate candidate = Candidate::none;
    unsigned consecutive = 0;
    unsigned read_failures = 0;
    unsigned temperature_divider = 0;
    for (;;) {
        float x = 0, y = 0, z = 0;
        // The component header declares qmi8658_read_accel_mps2(), but the
        // shipped 2.0.2 source does not define it. qmi8658_read_accel() uses
        // the unit flag configured during initialization and is implemented.
        const esp_err_t error = qmi8658_read_accel(&sensor, &x, &y, &z);
        if (error != ESP_OK) {
            if ((read_failures++ % 100) == 0) {
                ESP_LOGW(kTag, "QMI8658 accelerometer read failed: %s", esp_err_to_name(error));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        read_failures = 0;
        if (++temperature_divider >= kTemperatureSampleDivider) {
            temperature_divider = 0;
            float celsius = 0.0f;
            if (qmi8658_read_temp(&sensor, &celsius) == ESP_OK && std::isfinite(celsius)) {
                portENTER_CRITICAL(&temperature_lock);
                cached_temperature_c = celsius;
                cached_temperature_valid = true;
                portEXIT_CRITICAL(&temperature_lock);
            }
        }
        const Candidate observed = classify(x, y);
        if (observed == Candidate::none || observed == stable) {
            candidate = Candidate::none;
            consecutive = 0;
        } else if (observed != candidate) {
            candidate = observed;
            consecutive = 1;
        } else if (++consecutive >= kStableSamples) {
            stable = candidate;
            candidate = Candidate::none;
            consecutive = 0;
            const esp_err_t rotation_error = display::set_rotation(to_rotation(stable));
            if (rotation_error == ESP_OK) {
                ESP_LOGI(kTag, "Stable orientation changed to %d degrees", static_cast<int>(stable) * 90);
            } else {
                ESP_LOGE(kTag, "Display rotation failed: %s", esp_err_to_name(rotation_error));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}

namespace input {
esp_err_t start_orientation_monitor()
{
    if (task_started) return ESP_ERR_INVALID_STATE;
    const i2c_master_bus_handle_t bus = power_management::i2c_bus();
    if (bus == nullptr) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(qmi8658_init(&sensor, bus, QMI8658_ADDRESS_HIGH), kTag,
                        "QMI8658 initialization failed");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_range(&sensor, QMI8658_ACCEL_RANGE_4G), kTag,
                        "Accelerometer range setup failed");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_odr(&sensor, QMI8658_ACCEL_ODR_125HZ), kTag,
                        "Accelerometer rate setup failed");
    ESP_RETURN_ON_ERROR(qmi8658_enable_accel(&sensor, true), kTag,
                        "Accelerometer enable failed");
    qmi8658_set_accel_unit_mps2(&sensor, true);
    ESP_RETURN_ON_FALSE(xTaskCreate(orientation_task, "orientation", 4096, nullptr, 4, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Orientation task creation failed");
    task_started = true;
    ESP_LOGI(kTag, "QMI8658 orientation monitor ready (100 ms sampling, %u stable samples)", kStableSamples);
    return ESP_OK;
}

bool orientation_temperature_celsius(float &celsius)
{
    portENTER_CRITICAL(&temperature_lock);
    const bool valid = cached_temperature_valid;
    celsius = cached_temperature_c;
    portEXIT_CRITICAL(&temperature_lock);
    return valid;
}
}
