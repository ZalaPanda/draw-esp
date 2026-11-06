#include "brightness_button.h"
#include "button_debouncer.h"
#include "display/display_driver.h"
#include "system/board_config.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "brightness-button";
static_assert(pdMS_TO_TICKS(10) > 0, "Button polling requires a FreeRTOS tick rate of at least 100 Hz");
bool started = false;
bool indicator_visible = false;

void button_task(void *)
{
    input::ButtonDebouncer button(gpio_get_level(board::kUserButton) == 0,
                                  xTaskGetTickCount(), pdMS_TO_TICKS(50));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        bool pressed = false;
        if (button.sample(gpio_get_level(board::kUserButton) == 0, xTaskGetTickCount(), pressed)) {
            ESP_LOGI(kTag, "User button %s", pressed ? "pressed" : "released");
            if (pressed) {
                indicator_visible = !indicator_visible;
                const esp_err_t error = display::set_noise_indicator_visible(indicator_visible);
                if (error != ESP_OK) {
                    ESP_LOGE(kTag, "Indicator visibility toggle failed: %s; release and press to retry",
                             esp_err_to_name(error));
                }
            }
        }
    }
}
}

namespace input {
esp_err_t start_brightness_button()
{
    ESP_RETURN_ON_FALSE(!started, ESP_ERR_INVALID_STATE, kTag, "Brightness button already started");
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << board::kUserButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "User-button GPIO configuration failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(button_task, "brightness", 3072, nullptr, 3, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Brightness-button task creation failed");
    started = true;
    ESP_LOGI(kTag, "GPIO18 active-low button ready; debounced sound bar visibility toggle");
    return ESP_OK;
}
}
