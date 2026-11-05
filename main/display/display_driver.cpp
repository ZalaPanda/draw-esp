#include "display_driver.h"
#include "system/board_config.h"

#include <algorithm>
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

namespace {
constexpr char kTag[] = "display";
constexpr int kStripeRows = 16;
constexpr int kUploadBrightnessPercent = 100;
constexpr int kIdleBrightnessPercent = 20;
constexpr uint64_t kUploadBrightnessDurationUs = 30ULL * 1000 * 1000;
constexpr size_t kFramePixels = board::kDisplayWidth * board::kDisplayHeight;
constexpr size_t kStripePixels = board::kDisplayWidth * kStripeRows;
static_assert(kFramePixels * sizeof(uint16_t) == 460800);
static_assert(kStripePixels * sizeof(uint16_t) == 15360);
static_assert(board::kDisplayHeight % kStripeRows == 0);

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
esp_timer_handle_t brightness_timer = nullptr;

constexpr int next_brightness(int current)
{
    return current == 0 ? 100 : current - 10;
}

constexpr bool brightness_cycle_is_valid()
{
    constexpr int expected[] = {10, 0, 100, 90, 80, 70, 60, 50, 40, 30, 20};
    int current = 20;
    for (int value : expected) {
        current = next_brightness(current);
        if (current != value || current < 0 || current > 100) {
            return false;
        }
    }
    return true;
}
static_assert(brightness_cycle_is_valid());

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
            for (int x = 0; x < board::kDisplayWidth; ++x) {
                const int sx = static_cast<int>((static_cast<int64_t>(x) * width) / board::kDisplayWidth);
                framebuffer[y * board::kDisplayWidth + x] = pixel_at(sx, sy);
            }
        }
    } else {
        const double scale = source_fit == display::FitMode::contain
            ? std::min(static_cast<double>(board::kDisplayWidth) / width,
                       static_cast<double>(board::kDisplayHeight) / height)
            : std::max(static_cast<double>(board::kDisplayWidth) / width,
                       static_cast<double>(board::kDisplayHeight) / height);
        const int scaled_width = std::max(1, static_cast<int>(width * scale));
        const int scaled_height = std::max(1, static_cast<int>(height * scale));
        const int offset_x = (board::kDisplayWidth - scaled_width) / 2;
        const int offset_y = (board::kDisplayHeight - scaled_height) / 2;
        for (int y = 0; y < board::kDisplayHeight; ++y) {
            const int sy = static_cast<int>((static_cast<int64_t>(y - offset_y) * height) / scaled_height);
            if (sy < 0 || sy >= height) continue;
            for (int x = 0; x < board::kDisplayWidth; ++x) {
                const int sx = static_cast<int>((static_cast<int64_t>(x - offset_x) * width) / scaled_width);
                if (sx >= 0 && sx < width) framebuffer[y * board::kDisplayWidth + x] = pixel_at(sx, sy);
            }
        }
    }
}

void brightness_timer_callback(void *)
{
    if (!initialized) return;
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    const esp_err_t error = esp_lcd_panel_co5300_set_brightness(panel, kIdleBrightnessPercent);
    if (error == ESP_OK) {
        brightness_percent = kIdleBrightnessPercent;
        ESP_LOGI(kTag, "Upload brightness timeout: %d%%", brightness_percent);
    } else {
        ESP_LOGE(kTag, "Timed brightness reduction failed: %s", esp_err_to_name(error));
    }
    xSemaphoreGive(display_mutex);
}

esp_err_t render_image(const uint16_t *pixels, int width, int height, display::FitMode fit, bool new_upload)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    ESP_RETURN_ON_FALSE(pixels != nullptr && width > 0 && height > 0, ESP_ERR_INVALID_ARG, kTag,
                        "Invalid source image");
    if (new_upload && esp_timer_is_active(brightness_timer)) esp_timer_stop(brightness_timer);
    xSemaphoreTake(display_mutex, portMAX_DELAY);
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
    esp_err_t error = flush_framebuffer();
    if (error == ESP_OK && new_upload) {
        error = esp_lcd_panel_co5300_set_brightness(panel, kUploadBrightnessPercent);
        if (error == ESP_OK) brightness_percent = kUploadBrightnessPercent;
    }
    if (error == ESP_OK) error = esp_lcd_panel_disp_on_off(panel, true);
    xSemaphoreGive(display_mutex);
    if (error == ESP_OK && new_upload) {
        error = esp_timer_start_once(brightness_timer, kUploadBrightnessDurationUs);
    }
    return error;
}
static_assert(gray_rgb565(0) == 0x0000);
static_assert(gray_rgb565(255) == 0xFFFF);
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
    esp_timer_create_args_t brightness_timer_config = {};
    brightness_timer_config.callback = brightness_timer_callback;
    brightness_timer_config.dispatch_method = ESP_TIMER_TASK;
    brightness_timer_config.name = "image_brightness";
    brightness_timer_config.skip_unhandled_events = true;
    ESP_RETURN_ON_ERROR(esp_timer_create(&brightness_timer_config, &brightness_timer), kTag,
                        "Timed brightness timer creation failed");

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
    initialized = true;
    ESP_LOGI(kTag, "CO5300 QSPI initialized; 460800-byte PSRAM framebuffer, 15360-byte internal DMA buffer");
    return ESP_OK;
}

esp_err_t show_test_pattern()
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    constexpr uint16_t colors[] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    for (int y = 0; y < board::kDisplayHeight; ++y) {
        for (int x = 0; x < board::kDisplayWidth; ++x) {
            uint16_t color = colors[x / (board::kDisplayWidth / 8)];
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
    if (error == ESP_OK) error = esp_lcd_panel_disp_on_off(panel, true);
    xSemaphoreGive(display_mutex);
    return error;
}

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
        if (error == ESP_OK) error = esp_lcd_panel_disp_on_off(panel, true);
    }
    xSemaphoreGive(display_mutex);
    return error;
}

esp_err_t clear()
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    if (esp_timer_is_active(brightness_timer)) esp_timer_stop(brightness_timer);
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    std::memset(framebuffer, 0, kFramePixels * sizeof(uint16_t));
    if (source_image != nullptr) {
        heap_caps_free(source_image);
        source_image = nullptr;
        source_width = source_height = 0;
    }
    esp_err_t error = esp_lcd_panel_co5300_set_brightness(panel, kIdleBrightnessPercent);
    if (error == ESP_OK) brightness_percent = kIdleBrightnessPercent;
    if (error == ESP_OK) error = flush_framebuffer();
    xSemaphoreGive(display_mutex);
    return error;
}

esp_err_t decrease_brightness()
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, kTag, "Display is not initialized");
    if (esp_timer_is_active(brightness_timer)) esp_timer_stop(brightness_timer);
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    const int next = next_brightness(brightness_percent);
    const esp_err_t error = esp_lcd_panel_co5300_set_brightness(panel, next);
    if (error == ESP_OK) brightness_percent = next;
    ESP_LOGI(kTag, "Brightness: %d%%", brightness_percent);
    xSemaphoreGive(display_mutex);
    return error;
}
}
