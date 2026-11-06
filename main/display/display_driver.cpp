#include "display_driver.h"
#include "activity_state.h"
#include "system/board_config.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "display";
constexpr int kStripeRows = 20;
constexpr int kIndicatorWidth = 20;
constexpr int kIndicatorLeft = 10;
constexpr int kImageLeft = 0;
constexpr int kImageWidth = board::kDisplayWidth;
constexpr size_t kFramePixels = board::kDisplayWidth * board::kDisplayHeight;
constexpr size_t kStripePixels = board::kDisplayWidth * kStripeRows;
constexpr size_t kIndicatorPixels = kIndicatorWidth * board::kDisplayHeight;
static_assert(kFramePixels * sizeof(uint16_t) == 460800);
static_assert(kStripePixels * sizeof(uint16_t) == 19200);
static_assert(board::kDisplayHeight % kStripeRows == 0);
static_assert(kIndicatorPixels * sizeof(uint16_t) <= kStripePixels * sizeof(uint16_t));

// Board-specific CO5300 register values and delays verified against Waveshare's BSP.
constexpr uint8_t kPage20[] = {0x20};
constexpr uint8_t kReg19[] = {0x10};
constexpr uint8_t kReg1c[] = {0xA0};
constexpr uint8_t kPage0[] = {0x00};
constexpr uint8_t kRegC4[] = {0x80};
constexpr uint8_t kRgb565[] = {0x55};
constexpr uint8_t kBrightnessControl[] = {0x20};
constexpr uint8_t kFullBrightness[] = {0xFF};
constexpr uint8_t kWindow[] = {0x00, 0x00, 0x01, 0xDF};
constexpr uint8_t kOrientation[] = {0xA0};
const co5300_lcd_init_cmd_t kInitCommands[] = {
    {0x11, nullptr, 0, 600},
    {0xFE, kPage20, 1, 0},
    {0x19, kReg19, 1, 0},
    {0x1C, kReg1c, 1, 0},
    {0xFE, kPage0, 1, 0},
    {0xC4, kRegC4, 1, 0},
    {0x3A, kRgb565, 1, 0},
    {0x35, kPage0, 1, 0},
    {0x53, kBrightnessControl, 1, 0},
    {0x51, kFullBrightness, 1, 0},
    {0x63, kFullBrightness, 1, 0},
    {0x2A, kWindow, 4, 0},
    {0x2B, kWindow, 4, 0},
    {0x36, kOrientation, 1, 0},
    {0x29, nullptr, 0, 600},
};
esp_lcd_panel_handle_t panel = nullptr;
uint16_t *framebuffer = nullptr;
uint16_t *source_image = nullptr;
int source_width = 0;
int source_height = 0;
display::FitMode source_fit = display::FitMode::contain;
display::Rotation current_rotation = display::Rotation::deg0;
uint8_t *transfer_buffer = nullptr;
SemaphoreHandle_t transfer_done = nullptr;
SemaphoreHandle_t display_mutex = nullptr;
bool initialized = false;
int brightness_percent = 20;
bool panel_on = false;
bool indicator_visible = false;
bool indicator_dirty = false;
unsigned indicator_level = 0;
unsigned indicator_threshold = CONFIG_APP_WAKE_THRESHOLD;
QueueHandle_t activity_queue = nullptr;
QueueHandle_t indicator_queue = nullptr;
display::ActivityState activity{
    CONFIG_APP_ACTIVE_BRIGHTNESS,
    CONFIG_APP_IDLE_TIMEOUT_SECONDS * 1000000ULL,
    CONFIG_APP_BRIGHTNESS_RAMP_MS * 1000ULL,
};
struct IndicatorUpdate {
    unsigned level;
    unsigned threshold;
};

esp_err_t draw_indicator_locked();

esp_err_t set_panel_locked(bool on)
{
    if (panel_on == on) return ESP_OK;
    const esp_err_t error = esp_lcd_panel_disp_on_off(panel, on);
    if (error == ESP_OK) panel_on = on;
    return error;
}

esp_err_t set_brightness_locked(int value)
{
    if (brightness_percent == value) return ESP_OK;
    const esp_err_t error = esp_lcd_panel_co5300_set_brightness(
        panel, static_cast<uint8_t>(value));
    if (error == ESP_OK) {
        brightness_percent = value;
        ESP_LOGD(kTag, "Ramp %u step: elapsed=%" PRIu64 " ms applied=%d%% register[0x51]=0x%02X panel=%s",
                 activity.generation,
                 (static_cast<uint64_t>(esp_timer_get_time()) - activity.started_us) / 1000,
                 value, display::brightness_register(value), panel_on ? "on" : "off");
    }
    return error;
}

bool on_transfer_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(transfer_done, &task_woken);
    return task_woken == pdTRUE;
}

constexpr uint16_t gray_rgb565(unsigned gray)
{
    return static_cast<uint16_t>(((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3));
}

esp_err_t flush_framebuffer()
{
    for (int y = 0; y < board::kDisplayHeight; y += kStripeRows) {
        const uint16_t *source = framebuffer + y * board::kDisplayWidth;
        for (size_t i = 0; i < kStripePixels; ++i) {
            transfer_buffer[2 * i] = static_cast<uint8_t>(source[i] >> 8);
            transfer_buffer[2 * i + 1] = static_cast<uint8_t>(source[i]);
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, y, board::kDisplayWidth,
                                                     y + kStripeRows, transfer_buffer),
                            kTag, "Framebuffer stripe at y=%d failed", y);
        ESP_RETURN_ON_FALSE(xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE,
                            ESP_ERR_TIMEOUT, kTag, "DMA completion timed out at y=%d", y);
    }
    return ESP_OK;
}

void render_source()
{
    const bool quarter_turn = current_rotation == display::Rotation::deg90 ||
                              current_rotation == display::Rotation::deg270;
    const int width = quarter_turn ? source_height : source_width;
    const int height = quarter_turn ? source_width : source_height;
    const auto pixel_at = [&](int x, int y) -> uint16_t {
        int sx = x;
        int sy = y;
        switch (current_rotation) {
        case display::Rotation::deg90: sx = y; sy = source_height - 1 - x; break;
        case display::Rotation::deg180: sx = source_width - 1 - x; sy = source_height - 1 - y; break;
        case display::Rotation::deg270: sx = source_width - 1 - y; sy = x; break;
        case display::Rotation::deg0: break;
        }
        return source_image[sy * source_width + sx];
    };
    std::memset(framebuffer, 0, kFramePixels * sizeof(uint16_t));
    if (source_fit == display::FitMode::stretch) {
        for (int y = 0; y < board::kDisplayHeight; ++y) {
            const int sy = static_cast<int>((static_cast<int64_t>(y) * height) / board::kDisplayHeight);
            for (int x = kImageLeft; x < board::kDisplayWidth; ++x) {
                const int sx = static_cast<int>((static_cast<int64_t>(x - kImageLeft) * width) / kImageWidth);
                framebuffer[y * board::kDisplayWidth + x] = pixel_at(sx, sy);
            }
        }
    } else {
        const double scale = source_fit == display::FitMode::contain
            ? std::min(static_cast<double>(kImageWidth) / width,
                       static_cast<double>(board::kDisplayHeight) / height)
            : std::max(static_cast<double>(kImageWidth) / width,
                       static_cast<double>(board::kDisplayHeight) / height);
        const int scaled_width = std::max(1, static_cast<int>(width * scale));
        const int scaled_height = std::max(1, static_cast<int>(height * scale));
        const int offset_x = kImageLeft + (kImageWidth - scaled_width) / 2;
        const int offset_y = (board::kDisplayHeight - scaled_height) / 2;
        for (int y = 0; y < board::kDisplayHeight; ++y) {
            const int sy = static_cast<int>((static_cast<int64_t>(y - offset_y) * height) / scaled_height);
            if (sy < 0 || sy >= height) continue;
            for (int x = kImageLeft; x < board::kDisplayWidth; ++x) {
                const int sx = static_cast<int>((static_cast<int64_t>(x - offset_x) * width) / scaled_width);
                if (sx >= 0 && sx < width) framebuffer[y * board::kDisplayWidth + x] = pixel_at(sx, sy);
            }
        }
    }
}

esp_err_t render_image(const uint16_t *pixels, int width, int height, display::FitMode fit, bool new_upload)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    ESP_RETURN_ON_FALSE(pixels != nullptr && width > 0 && height > 0, ESP_ERR_INVALID_ARG, kTag,
                        "Invalid source image");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    esp_err_t error = ESP_OK;
    auto *replacement = static_cast<uint16_t *>(heap_caps_malloc(
        static_cast<size_t>(width) * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (replacement == nullptr) {
        xSemaphoreGive(display_mutex);
        return ESP_ERR_NO_MEM;
    }
    std::memcpy(replacement, pixels, static_cast<size_t>(width) * height * sizeof(uint16_t));
    if (source_image != nullptr) heap_caps_free(source_image);
    source_image = replacement;
    source_width = width;
    source_height = height;
    source_fit = fit;
    render_source();
    error = flush_framebuffer();
    indicator_dirty = true;
    if (error == ESP_OK && !new_upload) error = set_panel_locked(true);
    if (error == ESP_OK && new_upload) error = display::notify_activity("image upload");
    xSemaphoreGive(display_mutex);
    return error;
}
static_assert(gray_rgb565(0) == 0x0000);
static_assert(gray_rgb565(255) == 0xFFFF);

void display_control_task(void *)
{
    for (;;) {
        const char *reason = nullptr;
        bool requested = xQueueReceive(activity_queue, &reason, pdMS_TO_TICKS(20)) == pdTRUE;
        xSemaphoreTake(display_mutex, portMAX_DELAY);
        if (!requested) requested = xQueueReceive(activity_queue, &reason, 0) == pdTRUE;
        const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
        if (requested) {
            do {
                const bool was_ramping = activity.ramping;
                const unsigned previous_generation = activity.generation;
                const bool replaced = activity.wake(now, brightness_percent, reason);
                if (was_ramping && replaced) {
                    ESP_LOGI(kTag, "Ramp %u cancelled at applied=%d%% register[0x51]=0x%02X",
                             previous_generation, brightness_percent,
                             display::brightness_register(brightness_percent));
                }
                ESP_LOGI(kTag, "Activity: %s; ramp %u %s applied=%d%% target=%d%% duration=%u ms; idle deadline=%" PRIu64 " ms",
                         reason, activity.generation, replaced ? "requested" : "retained",
                         brightness_percent, activity.target,
                         CONFIG_APP_BRIGHTNESS_RAMP_MS, activity.deadline_us / 1000);
            } while (xQueueReceive(activity_queue, &reason, 0) == pdTRUE);
        } else if (activity.expire(now, brightness_percent)) {
            ESP_LOGI(kTag, "Ramp %u requested: idle timeout; applied=%d%% -> 0%% over %u ms",
                     activity.generation, brightness_percent, CONFIG_APP_BRIGHTNESS_RAMP_MS);
        }
        if (activity.ramping) {
            esp_err_t error = ESP_OK;
            if (activity.active && !panel_on) {
                error = set_brightness_locked(0);
                if (error == ESP_OK) {
                    activity.start = 0;
                    error = set_panel_locked(true);
                    if (error == ESP_OK) {
                        ESP_LOGI(kTag, "Ramp %u panel on commanded: %s; applied=0%% register[0x51]=0x00",
                                 activity.generation, activity.reason);
                    }
                }
            }
            if (error == ESP_OK) error = set_brightness_locked(activity.value(now));
            if (error == ESP_OK && (activity.finished(now) || activity.start == activity.target)) {
                if (!activity.active) error = set_panel_locked(false);
                if (error == ESP_OK) {
                    activity.ramping = false;
                    ESP_LOGI(kTag, "Ramp %u complete: %s; elapsed=%" PRIu64 " ms panel=%s applied=%d%% register[0x51]=0x%02X",
                             activity.generation, activity.reason, (now - activity.started_us) / 1000,
                             panel_on ? "on" : "off",
                             brightness_percent, display::brightness_register(brightness_percent));
                }
            }
            if (error != ESP_OK) {
                activity.ramping = false;
                ESP_LOGE(kTag, "Ramp %u failed (%s): %s; last applied=%d%% register[0x51]=0x%02X panel=%s; next activity retries",
                         activity.generation, activity.reason, esp_err_to_name(error),
                         brightness_percent, display::brightness_register(brightness_percent),
                         panel_on ? "on" : "off");
            }
        }
        IndicatorUpdate update = {};
        if (xQueueReceive(indicator_queue, &update, 0) == pdTRUE) {
            indicator_level = update.level;
            indicator_threshold = update.threshold;
            indicator_dirty = true;
        }
        if (indicator_visible && indicator_dirty && panel_on) {
            const esp_err_t error = draw_indicator_locked();
            indicator_dirty = false;
            if (error != ESP_OK) {
                ESP_LOGE(kTag, "Indicator transfer failed: %s", esp_err_to_name(error));
            }
        }
        xSemaphoreGive(display_mutex);
    }
}
}

namespace display {
esp_err_t initialize()
{
    ESP_RETURN_ON_FALSE(framebuffer == nullptr, ESP_ERR_INVALID_STATE, kTag, "Display already initialized");
    framebuffer = static_cast<uint16_t *>(heap_caps_calloc(kFramePixels, sizeof(uint16_t),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(framebuffer != nullptr, ESP_ERR_NO_MEM, kTag, "PSRAM framebuffer allocation failed");
    transfer_buffer = static_cast<uint8_t *>(heap_caps_malloc(kStripePixels * sizeof(uint16_t),
                                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(transfer_buffer != nullptr, ESP_ERR_NO_MEM, kTag, "Internal DMA buffer allocation failed");
    transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(transfer_done != nullptr, ESP_ERR_NO_MEM, kTag, "Transfer semaphore creation failed");
    display_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(display_mutex != nullptr, ESP_ERR_NO_MEM, kTag, "Display mutex creation failed");
    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = board::kDisplayClock;
    bus_config.data0_io_num = board::kDisplayData0;
    bus_config.data1_io_num = board::kDisplayData1;
    bus_config.data2_io_num = board::kDisplayData2;
    bus_config.data3_io_num = board::kDisplayData3;
    bus_config.data4_io_num = -1;
    bus_config.data5_io_num = -1;
    bus_config.data6_io_num = -1;
    bus_config.data7_io_num = -1;
    bus_config.max_transfer_sz = kStripePixels * sizeof(uint16_t);
    bus_config.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(board::kDisplayHost, &bus_config, SPI_DMA_CH_AUTO),
                        kTag, "QSPI bus initialization failed");

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = board::kDisplayCs;
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40000000;
    io_config.trans_queue_depth = 1;
    io_config.on_color_trans_done = on_transfer_done;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(board::kDisplayHost, &io_config, &io),
                        kTag, "QSPI panel IO creation failed");

    co5300_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = kInitCommands;
    vendor_config.init_cmds_size = sizeof(kInitCommands) / sizeof(kInitCommands[0]);
    vendor_config.flags.use_qspi_interface = true;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = board::kDisplayReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_config, &panel), kTag, "CO5300 creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), kTag, "CO5300 reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), kTag, "CO5300 initialization failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, false), kTag, "Initial display blanking failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(panel, brightness_percent),
                        kTag, "Brightness setting failed");
    activity_queue = xQueueCreate(8, sizeof(const char *));
    indicator_queue = xQueueCreate(1, sizeof(IndicatorUpdate));
    ESP_RETURN_ON_FALSE(activity_queue != nullptr && indicator_queue != nullptr,
                        ESP_ERR_NO_MEM, kTag, "Display queue creation failed");
    activity.deadline_us = static_cast<uint64_t>(esp_timer_get_time()) + activity.idle_us;
    initialized = true;
    ESP_RETURN_ON_FALSE(xTaskCreate(display_control_task, "display-control", 4096, nullptr, 4, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Display control task creation failed");
    ESP_LOGI(kTag, "CO5300 QSPI initialized; 460800-byte PSRAM framebuffer, 19200-byte internal DMA buffer");
    ESP_LOGI(kTag, "Startup: panel=off applied=%d%% register[0x51]=0x%02X; active=%u%% idle=%u s ramp=%u ms",
             brightness_percent, brightness_register(brightness_percent), CONFIG_APP_ACTIVE_BRIGHTNESS,
             CONFIG_APP_IDLE_TIMEOUT_SECONDS, CONFIG_APP_BRIGHTNESS_RAMP_MS);
    return ESP_OK;
}

esp_err_t show_test_pattern()
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    constexpr uint16_t colors[] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    for (int y = 0; y < board::kDisplayHeight; ++y) {
        for (int x = 0; x < board::kDisplayWidth; ++x) {
            if (x < kImageLeft) {
                framebuffer[y * board::kDisplayWidth + x] = 0;
                continue;
            }
            uint16_t color = colors[(x - kImageLeft) * 8 / kImageWidth];
            if (y >= board::kDisplayHeight / 2) {
                color = gray_rgb565(x * 255 / (board::kDisplayWidth - 1));
            }
            if ((x >= 238 && x < 242 && y >= 200 && y < 280) ||
                (y >= 238 && y < 242 && x >= 200 && x < 280)) {
                color = 0xFFFF;
            }
            framebuffer[y * board::kDisplayWidth + x] = color;
        }
    }

    // The CO5300 accepts RGB565 MSB first; keep the PSRAM framebuffer in native byte order.
    esp_err_t error = flush_framebuffer();
    if (error == ESP_OK) error = set_panel_locked(true);
    xSemaphoreGive(display_mutex);
    return error;
}

esp_err_t update_noise_indicator(unsigned level_percent, unsigned threshold_percent)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    const IndicatorUpdate update{std::min(level_percent, 100U), std::min(threshold_percent, 100U)};
    ESP_RETURN_ON_FALSE(xQueueOverwrite(indicator_queue, &update) == pdPASS,
                        ESP_FAIL, kTag, "Indicator update enqueue failed");
    return ESP_OK;
}
}

namespace {
esp_err_t draw_indicator_locked()
{
    constexpr uint16_t kBackground = 0x2104;
    constexpr uint16_t kBelowThreshold = 0x07E0;
    constexpr uint16_t kAboveThreshold = 0xF800;
    constexpr int kBarTopMargin = 10;
    constexpr int kBarBottomMargin = 10;
    constexpr int kBarHeight = board::kDisplayHeight - kBarTopMargin - kBarBottomMargin;
    const int fill_height = static_cast<int>((static_cast<uint64_t>(indicator_level) * kBarHeight) / 100);
    const uint16_t fill_color = indicator_level >= indicator_threshold ? kAboveThreshold : kBelowThreshold;
    for (int y = 0; y < kBarHeight; ++y) {
        uint16_t color = kBackground;
        if (y >= kBarHeight - fill_height) {
            color = fill_color;
        }
        const size_t pixel_index = static_cast<size_t>(y) * kIndicatorWidth;
        for (int x = 0; x < kIndicatorWidth; ++x) {
            const size_t byte_index = (pixel_index + x) * 2;
            transfer_buffer[byte_index] = static_cast<uint8_t>(color >> 8);
            transfer_buffer[byte_index + 1] = static_cast<uint8_t>(color);
        }
    }

    const esp_err_t error = esp_lcd_panel_draw_bitmap(panel, kIndicatorLeft, kBarTopMargin,
                                                      kIndicatorLeft + kIndicatorWidth,
                                                      board::kDisplayHeight - kBarBottomMargin,
                                                      transfer_buffer);
    if (error == ESP_OK && xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return error;
}
}

namespace display {
esp_err_t show_rgb565(const uint16_t *pixels, int width, int height, FitMode fit)
{
    return render_image(pixels, width, height, fit, true);
}

esp_err_t restore_rgb565(const uint16_t *pixels, int width, int height, FitMode fit)
{
    return render_image(pixels, width, height, fit, false);
}

esp_err_t set_rotation(Rotation rotation)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    current_rotation = rotation;
    esp_err_t error = ESP_OK;
    if (source_image != nullptr) {
        render_source();
        error = flush_framebuffer();
        indicator_dirty = true;
    }
    if (error == ESP_OK) error = notify_activity("orientation change");
    xSemaphoreGive(display_mutex);
    return error;
}

esp_err_t clear()
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    std::memset(framebuffer, 0, kFramePixels * sizeof(uint16_t));
    if (source_image != nullptr) {
        heap_caps_free(source_image);
        source_image = nullptr;
        source_width = source_height = 0;
    }
    esp_err_t error = flush_framebuffer();
    indicator_dirty = true;
    if (error == ESP_OK) error = notify_activity("image clear");
    xSemaphoreGive(display_mutex);
    return error;
}

esp_err_t notify_activity(const char *reason)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    ESP_RETURN_ON_FALSE(reason != nullptr, ESP_ERR_INVALID_ARG, kTag, "Activity reason is required");
    ESP_RETURN_ON_FALSE(xQueueSend(activity_queue, &reason, 0) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Activity queue full: %s", reason);
    return ESP_OK;
}

esp_err_t set_noise_indicator_visible(bool visible)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    indicator_visible = visible;
    indicator_dirty = true;
    if (visible) {
        ESP_LOGI(kTag, "Sound bar visible; requesting activity wake");
        const esp_err_t error = notify_activity("sound bar shown");
        xSemaphoreGive(display_mutex);
        return error;
    }
    for (int y = 0; y < board::kDisplayHeight; ++y) {
        for (int x = 0; x < kIndicatorWidth; ++x) {
            const uint16_t pixel = framebuffer[y * board::kDisplayWidth + kIndicatorLeft + x];
            const size_t byte_index = (static_cast<size_t>(y) * kIndicatorWidth + x) * 2;
            transfer_buffer[byte_index] = static_cast<uint8_t>(pixel >> 8);
            transfer_buffer[byte_index + 1] = static_cast<uint8_t>(pixel);
        }
    }
    esp_err_t error = esp_lcd_panel_draw_bitmap(panel, kIndicatorLeft, 0,
                                                kIndicatorLeft + kIndicatorWidth,
                                                board::kDisplayHeight, transfer_buffer);
    if (error == ESP_OK && xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        error = ESP_ERR_TIMEOUT;
    }
    if (error == ESP_OK) {
        ESP_LOGI(kTag, "Sound bar hidden; requesting activity wake");
        error = notify_activity("sound bar hidden");
    }
    xSemaphoreGive(display_mutex);
    return error;
}
}
