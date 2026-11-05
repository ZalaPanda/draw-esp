#include "noise_level.h"
#include "display/display_driver.h"
#include "system/board_config.h"
#include "system/power_management.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "noise-level";
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kWindowFrames = 320;
constexpr size_t kChannels = 2;
constexpr size_t kWindowSamples = kWindowFrames * kChannels;
constexpr float kThresholdDb = 12.0F;
constexpr float kDisplayRangeDb = 30.0F;
constexpr uint8_t kMicrophones = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;

i2s_chan_handle_t microphone_i2s = nullptr;
esp_codec_dev_handle_t microphone = nullptr;
bool monitor_started = false;

esp_err_t initialize_microphone()
{
    const i2c_master_bus_handle_t i2c_bus = power_management::i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != nullptr, ESP_ERR_INVALID_STATE, kTag,
                        "Shared I2C bus is not initialized");

    const i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        board::kAudioI2sPort, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, nullptr, &microphone_i2s), kTag,
                        "Microphone I2S channel creation failed");

    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = board::kAudioMclk,
            .bclk = board::kAudioBclk,
            .ws = board::kAudioLrck,
            .dout = I2S_GPIO_UNUSED,
            .din = board::kMicrophoneData,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(microphone_i2s, &i2s_config), kTag,
                        "Microphone I2S configuration failed");

    audio_codec_i2c_cfg_t i2c_config = {
        .port = I2C_NUM_0,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(control != nullptr, ESP_FAIL, kTag,
                        "Microphone I2C control interface creation failed");

    audio_codec_i2s_cfg_t data_config = {
        .port = board::kAudioI2sPort,
        .rx_handle = microphone_i2s,
        .tx_handle = nullptr,
        .clk_src = 0,
    };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_config);
    ESP_RETURN_ON_FALSE(data != nullptr, ESP_FAIL, kTag,
                        "Microphone I2S data interface creation failed");

    es7210_codec_cfg_t es7210_config = {
        .ctrl_if = control,
        .master_mode = false,
        .mic_selected = kMicrophones,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec = es7210_codec_new(&es7210_config);
    ESP_RETURN_ON_FALSE(codec != nullptr, ESP_FAIL, kTag,
                        "ES7210 codec interface creation failed");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec,
        .data_if = data,
    };
    microphone = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(microphone != nullptr, ESP_FAIL, kTag,
                        "Microphone codec device creation failed");

    esp_codec_dev_sample_info_t sample_config = {
        .bits_per_sample = 16,
        .channel = kChannels,
        .channel_mask = kMicrophones,
        .sample_rate = kSampleRate,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(microphone, &sample_config) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, kTag, "ES7210 open failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(microphone, 24.0F) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, kTag, "ES7210 input gain setup failed");
    ESP_LOGI(kTag, "ES7210 capture ready: two microphones, %u Hz, 16-bit PCM",
             static_cast<unsigned>(kSampleRate));
    return ESP_OK;
}

void noise_level_task(void *)
{
    int16_t samples[kWindowSamples] = {};
    float noise_floor = 0.0F;
    bool have_noise_floor = false;
    unsigned elevated_windows = 0;
    unsigned read_failures = 0;

    for (;;) {
        const int result = esp_codec_dev_read(microphone, samples, sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            if ((read_failures++ % 50) == 0) {
                ESP_LOGW(kTag, "Microphone read failed: %d", result);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        read_failures = 0;

        double sum_squares = 0.0;
        for (const int16_t sample : samples) {
            const double normalized = static_cast<double>(sample) / 32768.0;
            sum_squares += normalized * normalized;
        }
        const float rms = static_cast<float>(std::sqrt(sum_squares / kWindowSamples));
        if (!have_noise_floor) {
            noise_floor = std::max(rms, 1.0e-6F);
            have_noise_floor = true;
        } else if (rms < noise_floor) {
            elevated_windows = 0;
            noise_floor += (rms - noise_floor) * 0.04F;
        } else if (rms < noise_floor * 3.0F) {
            elevated_windows = 0;
            // Slowly follow a sustained rise in room noise while ignoring loud transients.
            noise_floor += (rms - noise_floor) * 0.002F;
        } else if (++elevated_windows >= 100) {
            // A level over 3x baseline must persist for 2 s before it can redefine the floor.
            noise_floor += (rms - noise_floor) * 0.002F;
        }

        const float relative_db = 20.0F * std::log10(std::max(rms, 1.0e-6F) /
                                                     std::max(noise_floor, 1.0e-6F));
        const float clamped_db = std::clamp(relative_db, 0.0F, kDisplayRangeDb);
        const unsigned level_percent = static_cast<unsigned>(clamped_db * 100.0F /
                                                              kDisplayRangeDb + 0.5F);
        const bool over_threshold = relative_db >= kThresholdDb;
        const esp_err_t draw_error = display::update_noise_indicator(level_percent, over_threshold);
        if (draw_error != ESP_OK) {
            ESP_LOGW(kTag, "Noise indicator update failed: %s", esp_err_to_name(draw_error));
        }
    }
}
}

namespace input {
esp_err_t start_noise_level_monitor()
{
    ESP_RETURN_ON_FALSE(!monitor_started, ESP_ERR_INVALID_STATE, kTag,
                        "Noise-level monitor already started");
    ESP_RETURN_ON_ERROR(initialize_microphone(), kTag, "Microphone initialization failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(noise_level_task, "noise-level", 4096, nullptr, 4, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Noise-level task creation failed");
    monitor_started = true;
    return ESP_OK;
}
}
