#include "esp_log.h"

namespace {
constexpr char kTag[] = "draw-esp";
}

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "Firmware scaffold started; board initialization is not configured yet.");
}
