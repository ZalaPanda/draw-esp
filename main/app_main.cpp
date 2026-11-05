#include "display/display_driver.h"
#include "input/brightness_button.h"
#include "system/board_config.h"
#include "system/power_management.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_psram.h"

namespace {
constexpr char kTag[] = "draw-esp";
}

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "Stage 1: Waveshare ESP32-S3-Touch-AMOLED-2.16 initialization");

    // ESP-IDF initializes and tests PSRAM before app_main.
    ESP_ERROR_CHECK(esp_psram_is_initialized() ? ESP_OK : ESP_ERR_INVALID_STATE);
    const size_t psram_bytes = esp_psram_get_size();
    ESP_LOGI(kTag, "PSRAM detected: %u bytes", static_cast<unsigned>(psram_bytes));
    ESP_ERROR_CHECK(psram_bytes == board::kPsramBytes ? ESP_OK : ESP_ERR_INVALID_SIZE);

    ESP_ERROR_CHECK(power_management::initialize());
    ESP_ERROR_CHECK(display::initialize());
    ESP_ERROR_CHECK(display::show_test_pattern());
    ESP_LOGI(kTag, "Stage 1 ready: RGB565 color bars, center cross and grayscale at 20%% brightness");
    ESP_ERROR_CHECK(input::start_brightness_button());
    ESP_LOGI(kTag, "Stage 1A ready: user button cycles brightness by -10%%, wrapping 0%% to 100%%");
}
