#include "noise_level.h"
#include "sound_events.h"
#include "display/display_driver.h"
#include "system/board_config.h"
#include "system/power_management.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

namespace {
constexpr char kTag[] = "noise-level";
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kWindowFrames = 320;
constexpr size_t kChannels = 2;
constexpr size_t kWindowSamples = kWindowFrames * kChannels;
constexpr float kDisplayRangeDb = 30.0F;
constexpr uint8_t kMicrophones = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
constexpr input::SoundSettings kSoundSettings{
    CONFIG_APP_WAKE_THRESHOLD, CONFIG_APP_REARM_THRESHOLD, CONFIG_APP_EVENT_QUALIFY_MS,
    CONFIG_APP_REARM_DURATION_MS, CONFIG_APP_REQUIRED_EVENTS, CONFIG_APP_EVENT_WINDOW_SECONDS * 1000,
};
static_assert(kSoundSettings.valid(), "Invalid sound settings: require 0 < re-arm < wake, and a feasible event window");

i2s_chan_handle_t microphone_i2s = nullptr;
esp_codec_dev_handle_t microphone = nullptr;
bool monitor_started = false;
portMUX_TYPE capture_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t capture_overflows = 0;

bool IRAM_ATTR on_capture_overflow(i2s_chan_handle_t, i2s_event_data_t *, void *)
{
    portENTER_CRITICAL_ISR(&capture_mux);
    ++capture_overflows;
    portEXIT_CRITICAL_ISR(&capture_mux);
    return false;
}

esp_err_t read_window(int16_t *samples)
{
    size_t bytes_read = 0;
    const esp_err_t error = i2s_channel_read(
        microphone_i2s, samples, kWindowSamples * sizeof(int16_t), &bytes_read, 1000);
    if (error != ESP_OK) return error;
    return bytes_read == kWindowSamples * sizeof(int16_t) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

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
    const i2s_event_callbacks_t callbacks = {
        .on_recv = nullptr,
        .on_recv_q_ovf = on_capture_overflow,
        .on_sent = nullptr,
        .on_send_q_ovf = nullptr,
    };
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(microphone_i2s, &callbacks, nullptr), kTag,
                        "Microphone overflow callback registration failed");

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
    // The codec first disables RX to change its format, then enables it again.
    ESP_RETURN_ON_ERROR(i2s_channel_enable(microphone_i2s), kTag, "Initial microphone RX enable failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(microphone, &sample_config) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, kTag, "ES7210 open failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(microphone, 24.0F) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, kTag, "ES7210 input gain setup failed");
    int16_t probe[kWindowSamples] = {};
    ESP_RETURN_ON_ERROR(read_window(probe), kTag, "ES7210 capture readiness check failed");
    // Overruns during codec setup are expected; only report those after capture is ready.
    portENTER_CRITICAL(&capture_mux);
    capture_overflows = 0;
    portEXIT_CRITICAL(&capture_mux);
    ESP_LOGI(kTag, "ES7210 capture ready: two microphones, %u Hz, 16-bit PCM, 24 dB gain; full window read verified",
             static_cast<unsigned>(kSampleRate));
    return ESP_OK;
}

void noise_level_task(void *)
{
    int16_t samples[kWindowSamples] = {};
    input::AmbientBaseline baseline;
    input::SoundEvents events(kSoundSettings);
    unsigned read_failures = 0;
    uint64_t previous_read_us = 0;
    uint64_t last_gap_log_us = 0;
    unsigned silent_windows = 0;
    bool silence_reported = false;

    for (;;) {
        const esp_err_t result = read_window(samples);
        const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
        if (result != ESP_OK) {
            events.discontinuity();
            silent_windows = 0;
            previous_read_us = 0;
            if ((read_failures++ % 50) == 0) {
                ESP_LOGE(kTag, "Microphone full-window read failed: %s; qualification/re-arm continuity reset",
                         esp_err_to_name(result));
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (read_failures != 0) ESP_LOGI(kTag, "Microphone capture recovered");
        read_failures = 0;
        portENTER_CRITICAL(&capture_mux);
        const uint32_t overflows = capture_overflows;
        capture_overflows = 0;
        portEXIT_CRITICAL(&capture_mux);
        if (overflows != 0 || (previous_read_us != 0 && now - previous_read_us > 60000)) {
            events.discontinuity();
            silent_windows = 0;
            if (last_gap_log_us == 0 || now - last_gap_log_us >= 1000000) {
                ESP_LOGW(kTag, "Capture discontinuity: overflows=%" PRIu32 " gap=%" PRIu64 " ms; continuity reset",
                         overflows, previous_read_us == 0 ? 0 : (now - previous_read_us) / 1000);
                last_gap_log_us = now;
            }
        }
        previous_read_us = now;

        double sum_squares = 0.0;
        unsigned nonzero = 0;
        for (const int16_t sample : samples) {
            if (sample != 0) ++nonzero;
            const double normalized = static_cast<double>(sample) / 32768.0;
            sum_squares += normalized * normalized;
        }
        const float rms = static_cast<float>(std::sqrt(sum_squares / kWindowSamples));
        if (nonzero == 0) {
            if (silent_windows < 250) ++silent_windows;
            if (silent_windows == 250 && !silence_reported) {
                ESP_LOGW(kTag, "Microphone PCM has been exactly zero for 5 seconds; check codec/capture");
                silence_reported = true;
            }
        } else {
            if (silence_reported) ESP_LOGI(kTag, "Nonzero microphone PCM recovered");
            silent_windows = 0;
            silence_reported = false;
        }
        if (!baseline.initialized()) {
            baseline.observe(rms, false);
            if (!baseline.initialized()) continue;
            ESP_LOGI(kTag, "Ambient baseline initialized: RMS=%.6f (floor %.6f)",
                     static_cast<double>(baseline.value()),
                     static_cast<double>(input::AmbientBaseline::kFloor));
        }

        const float reference = baseline.value();
        const float relative_db = 20.0F * std::log10(std::max(rms, 1.0e-6F) /
                                                     std::max(reference, 1.0e-6F));
        const float clamped_db = std::clamp(relative_db, 0.0F, kDisplayRangeDb);
        const unsigned level_percent = static_cast<unsigned>(clamped_db * 100.0F /
                                                              kDisplayRangeDb + 0.5F);
        const auto event = events.sample(level_percent, 20, now);
        baseline.observe(rms, events.protect_baseline());
        if (event.expired != 0) {
            ESP_LOGI(kTag, "Sound event window expired: removed=%u remaining=%u/%u",
                     event.expired, event.count, CONFIG_APP_REQUIRED_EVENTS);
        }
        if (event.qualified) {
            ESP_LOGI(kTag, "Qualified sound event %u/%u: level=%u%% relative=%.1f dB; window remaining=%u ms",
                     event.count, CONFIG_APP_REQUIRED_EVENTS, level_percent,
                     static_cast<double>(relative_db), event.remaining_ms);
            if (event.activity) {
                const esp_err_t error = display::notify_activity("qualified sound events");
                if (error != ESP_OK) ESP_LOGE(kTag, "Sound activity request failed: %s", esp_err_to_name(error));
            }
        }
        if (event.rearmed) ESP_LOGI(kTag, "Sound detector re-armed: level=%u%%", level_percent);
        if (event.hold_expired) {
            ESP_LOGI(kTag, "No re-arm within %u ms (level=%u%%); baseline adaptation resumed",
                     input::kMaxBaselineHoldMs, level_percent);
        }
        if (event.rejected) ESP_LOGI(kTag, "Sound candidate rejected: level=%u%% below wake=%u%%",
                                    level_percent, CONFIG_APP_WAKE_THRESHOLD);

        const esp_err_t draw_error = display::update_noise_indicator(level_percent, CONFIG_APP_WAKE_THRESHOLD);
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
    ESP_LOGI(kTag, "Sound settings: wake=%u%% (+%.1f dB), re-arm<%u%%, qualify=%u ms re-arm=%u ms; %u events/%u s",
             CONFIG_APP_WAKE_THRESHOLD, CONFIG_APP_WAKE_THRESHOLD * 0.3,
             CONFIG_APP_REARM_THRESHOLD, CONFIG_APP_EVENT_QUALIFY_MS,
             CONFIG_APP_REARM_DURATION_MS, CONFIG_APP_REQUIRED_EVENTS, CONFIG_APP_EVENT_WINDOW_SECONDS);
    ESP_RETURN_ON_FALSE(xTaskCreate(noise_level_task, "noise-level", 4096, nullptr, 4, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Noise-level task creation failed");
    monitor_started = true;
    return ESP_OK;
}
}
