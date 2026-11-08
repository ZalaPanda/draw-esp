#include "touch_brightness.h"
#include "touch_gesture.h"
#include "display/display_driver.h"
#include "system/board_config.h"
#include "system/power_management.h"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "touch-brightness";
constexpr uint64_t kMaxSampleGapUs = 200000;
constexpr uint64_t kHoldActivityUs = 1000000;
esp_lcd_panel_io_handle_t touch_io = nullptr;
esp_lcd_touch_handle_t touch = nullptr;
bool started = false;
static_assert(pdMS_TO_TICKS(20) > 0);

esp_err_t read_report(input::TouchReport &report)
{
    // Vendor D000 report, preserving the point count its one-point API truncates.
    const uint8_t register_low = 0;
    esp_err_t error = esp_lcd_panel_io_tx_param(touch_io, 0xD0, &register_low, 1);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(2));
    uint8_t data[10] = {};
    error = esp_lcd_panel_io_rx_param(touch_io, -1, data, sizeof(data));
    if (error != ESP_OK) return error;
    return input::decode_touch_report(data, report) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

void touch_task(void *)
{
    input::TouchGesture gesture;
    uint64_t last_sample = 0;
    uint64_t last_activity = 0;
    uint64_t last_error_log = 0;
    bool failed = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        input::TouchReport report;
        esp_err_t error = read_report(report);
        const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
        if (error == ESP_OK && last_sample != 0 && now - last_sample > kMaxSampleGapUs) {
            ESP_LOGW(kTag, "Touch sample gap; cancelling swipe");
            gesture.cancel();
        }
        last_sample = now;
        display::TouchState state = {};
        if (error == ESP_OK) error = display::read_touch_state(state);
        if (error == ESP_OK && gesture.needs_begin(report.points)) {
            error = display::begin_touch(state);
            if (error == ESP_OK) {
                gesture.begin(report.point, static_cast<unsigned>(state.rotation),
                              state.rotation_generation, !state.panel_on);
                last_activity = now;
            }
        } else if (error == ESP_OK && report.points != 0 && now - last_activity >= kHoldActivityUs) {
            error = display::notify_activity("touch held");
            if (error == ESP_OK) last_activity = now;
        }
        if (error == ESP_OK) {
            const int delta = gesture.sample(report.points, report.point, state.rotation_generation);
            if (delta != 0) error = display::adjust_brightness(delta, gesture.generation());
        }
        const esp_err_t preview_error = display::update_brightness_preview(
            error == ESP_OK && report.points <= 1 && gesture.adjusting(),
            gesture.preview_delta(), gesture.generation());
        if (error == ESP_OK) error = preview_error;
        else if (preview_error != ESP_OK) {
            ESP_LOGE(kTag, "Brightness preview cancel failed: %s", esp_err_to_name(preview_error));
        }
        if (error != ESP_OK) {
            gesture.cancel();
            if (last_error_log == 0 || now - last_error_log >= 5000000) {
                ESP_LOGE(kTag, "Touch unavailable/swipe cancelled: %s; release before retry",
                         esp_err_to_name(error));
                last_error_log = now;
            }
            failed = true;
        } else if (failed) {
            ESP_LOGI(kTag, "Touch reads recovered; waiting for release before accepting swipes");
            failed = false;
        }
    }
}
}

namespace input {
esp_err_t start_touch_brightness()
{
    ESP_RETURN_ON_FALSE(!started, ESP_ERR_INVALID_STATE, kTag, "Touch already started");
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 8;
    io_config.flags.disable_control_phase = true;
    io_config.scl_speed_hz = 400000;
    io_config.transaction_timeout_ms = 100;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(power_management::i2c_bus(), &io_config, &touch_io),
                        kTag, "Touch I2C IO creation failed");
    esp_lcd_touch_config_t config = {};
    config.x_max = board::kDisplayWidth;
    config.y_max = board::kDisplayHeight;
    config.rst_gpio_num = board::kTouchReset;
    config.int_gpio_num = board::kTouchInterrupt;
    config.levels.reset = 0;
    config.levels.interrupt = 0;
    config.flags.swap_xy = true;
    config.flags.mirror_y = true;
    esp_err_t error = esp_lcd_touch_new_i2c_cst9217(touch_io, &config, &touch);
    if (error == ESP_OK && xTaskCreate(touch_task, "touch", 3072, nullptr, 3, nullptr) != pdPASS) {
        error = ESP_ERR_NO_MEM;
    }
    if (error != ESP_OK) {
        if (touch != nullptr) {
            const esp_err_t cleanup = esp_lcd_touch_del(touch);
            if (cleanup != ESP_OK) ESP_LOGE(kTag, "Touch cleanup failed: %s", esp_err_to_name(cleanup));
            touch = nullptr;
        }
        const esp_err_t cleanup = esp_lcd_panel_io_del(touch_io);
        if (cleanup != ESP_OK) ESP_LOGE(kTag, "Touch IO cleanup failed: %s", esp_err_to_name(cleanup));
        touch_io = nullptr;
        ESP_LOGE(kTag, "Touch startup failed: %s", esp_err_to_name(error));
        return error;
    }
    started = true;
    ESP_LOGI(kTag, "CST9220 at 0x5A ready; vertical swipes adjust saved brightness 10-100%%");
    return ESP_OK;
}
}
