#pragma once

#include "esp_err.h"
#include <cstdint>

namespace display {
enum class FitMode { contain, cover, stretch };
enum class Rotation : uint8_t { deg0, deg90, deg180, deg270 };
esp_err_t initialize(unsigned brightness_percent);
esp_err_t show_test_pattern();
esp_err_t show_rgb565(const uint16_t *pixels, int width, int height, FitMode fit);
esp_err_t restore_rgb565(const uint16_t *pixels, int width, int height, FitMode fit);
esp_err_t set_rotation(Rotation rotation);
esp_err_t clear();
// The reason must have static storage; success means queued, not physically completed.
esp_err_t notify_activity(const char *reason);
esp_err_t update_noise_indicator(unsigned level_percent, unsigned threshold_percent);
esp_err_t set_noise_indicator_visible(bool visible);
struct TouchState {
    bool panel_on;
    Rotation rotation;
    unsigned rotation_generation;
};
esp_err_t read_touch_state(TouchState &state);
esp_err_t begin_touch(TouchState &state);
esp_err_t adjust_brightness(int delta, unsigned rotation_generation);
// Applies the in-progress swipe level to the panel immediately and shows the right-side bar.
esp_err_t update_brightness_preview(bool adjusting, int preview_delta, unsigned rotation_generation);
}
