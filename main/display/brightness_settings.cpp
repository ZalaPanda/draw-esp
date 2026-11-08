#include "brightness_settings.h"
#include "brightness_level.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr char kTag[] = "brightness-settings";
constexpr char kNamespace[] = "display";
constexpr char kKey[] = "brightness";
unsigned saved_percent = CONFIG_APP_ACTIVE_BRIGHTNESS;
bool initialized = false;
static_assert(display::valid_brightness(CONFIG_APP_ACTIVE_BRIGHTNESS));
}

namespace display {
esp_err_t initialize_brightness_settings(unsigned &percent)
{
    ESP_RETURN_ON_FALSE(!initialized, ESP_ERR_INVALID_STATE, kTag, "Settings already initialized");
    ESP_RETURN_ON_ERROR(nvs_flash_init(), kTag,
                        "NVS initialization failed; refusing to erase saved data");
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag, "Settings open failed");
    uint8_t stored = 0;
    const esp_err_t error = nvs_get_u8(handle, kKey, &stored);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "No saved brightness; using default %u%%", saved_percent);
    } else if (error == ESP_ERR_NVS_TYPE_MISMATCH || (error == ESP_OK && !valid_brightness(stored))) {
        ESP_LOGW(kTag, "Invalid saved brightness (%s, value=%u); using default %u%%",
                 esp_err_to_name(error), stored, saved_percent);
    } else {
        ESP_RETURN_ON_ERROR(error, kTag, "Brightness read failed");
        saved_percent = stored;
        ESP_LOGI(kTag, "Restored selected brightness %u%%", saved_percent);
    }
    percent = saved_percent;
    initialized = true;
    return ESP_OK;
}

esp_err_t save_brightness(unsigned percent)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Settings not initialized");
    ESP_RETURN_ON_FALSE(valid_brightness(percent), ESP_ERR_INVALID_ARG, kTag,
                        "Brightness must be 10-100%%");
    if (percent == saved_percent) return ESP_OK;
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag, "Settings open failed");
    esp_err_t error = nvs_set_u8(handle, kKey, static_cast<uint8_t>(percent));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(error, kTag, "Brightness save failed; selection remains %u%%", saved_percent);
    saved_percent = percent;
    ESP_LOGI(kTag, "Saved selected brightness %u%%", percent);
    return ESP_OK;
}
}
