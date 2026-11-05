#include "display/display_driver.h"
#include "image/image_decoder.h"
#include "image/image_store.h"
#include "input/brightness_button.h"
#include "input/orientation.h"
#include "input/noise_level.h"
#include "network/network_api.h"
#include "network/network_manager.h"
#include "system/board_config.h"
#include "system/power_management.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"

#include <cstring>

namespace {
constexpr char kTag[] = "draw-esp";

void on_power_key_short_press()
{
    const esp_err_t error = network::toggle_ap();
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "PWR short-press AP toggle failed: %s", esp_err_to_name(error));
    }
}

bool restore_last_image()
{
    uint8_t *stored = nullptr;
    size_t stored_size = 0;
    display::FitMode fit = display::FitMode::contain;
    const esp_err_t load_error = image_store::load(&stored, &stored_size, &fit);
    if (load_error == ESP_ERR_NOT_FOUND) return false;
    if (load_error != ESP_OK) {
        ESP_LOGW(kTag, "Saved image unavailable (%s); showing test pattern", esp_err_to_name(load_error));
        return false;
    }

    image::DecodedImage decoded = {};
    esp_err_t error = ESP_ERR_INVALID_RESPONSE;
    if (stored_size >= 3 && stored[0] == 0xFF && stored[1] == 0xD8 && stored[2] == 0xFF) {
        error = image::decode_jpeg(stored, stored_size, decoded);
    } else if (stored_size >= 8 && std::memcmp(stored, "\x89PNG\r\n\x1A\n", 8) == 0) {
        error = image::decode_png(stored, stored_size, decoded);
    } else if (stored_size >= 2 && stored[0] == 'B' && stored[1] == 'M') {
        error = image::decode_bmp(stored, stored_size, decoded);
    }
    heap_caps_free(stored);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "Saved image cannot be decoded (%s); showing test pattern", esp_err_to_name(error));
        return false;
    }
    error = display::restore_rgb565(decoded.pixels, decoded.width, decoded.height, fit);
    image::release(decoded);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "Saved image render failed (%s); showing test pattern", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(kTag, "Restored saved image at default 20%% brightness");
    return true;
}
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
    ESP_ERROR_CHECK(image_store::initialize());
    if (!restore_last_image()) {
        ESP_ERROR_CHECK(display::show_test_pattern());
        ESP_LOGI(kTag, "No saved image; showing the Stage 1 test pattern at 20%% brightness");
    }

    ESP_ERROR_CHECK(network::initialize());
    ESP_ERROR_CHECK(network::start_http_api());
    ESP_LOGI(kTag, "Stage 3 ready: AP+STA, persistent Wi-Fi credentials, reconnect and mDNS");
    ESP_ERROR_CHECK(power_management::start_power_key_monitor(on_power_key_short_press));
    ESP_LOGI(kTag, "Stage 2 ready: PWR short press toggles AP; long-press PMIC behavior is unchanged");

    ESP_ERROR_CHECK(input::start_brightness_button());
    ESP_LOGI(kTag, "Stage 1A ready: user button cycles brightness by -10%%, wrapping 0%% to 100%%");

    ESP_ERROR_CHECK(input::start_orientation_monitor());
    ESP_LOGI(kTag, "Stage 5 ready: QMI8658 stable-orientation image rotation enabled");

    const esp_err_t noise_error = input::start_noise_level_monitor();
    if (noise_error == ESP_OK) {
        ESP_LOGI(kTag, "Stage 7 ready: microphone noise-level indicator active");
    } else {
        ESP_LOGE(kTag, "Noise-level indicator unavailable: %s", esp_err_to_name(noise_error));
    }
}
