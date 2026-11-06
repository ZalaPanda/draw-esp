#pragma once

#include <array>
#include <cstdint>

namespace input {
struct SoundSettings {
    unsigned wake;
    unsigned rearm;
    unsigned qualify_ms;
    unsigned rearm_ms;
    unsigned required;
    unsigned window_ms;

    constexpr bool valid() const
    {
        return wake >= 1 && wake <= 100 && rearm >= 1 && rearm < wake && window_ms > 0 &&
               qualify_ms >= 20 && rearm_ms >= 20 && required >= 1 && required <= 10 &&
               window_ms >= (required - 1) *
                   (((qualify_ms + 19) / 20) * 20 + ((rearm_ms + 19) / 20) * 20);
    }
};

// Bounds how long a qualified event may freeze the ambient baseline. Without a bound, a
// baseline that is too low keeps the level above re-arm forever, which never unfreezes it.
constexpr unsigned kMaxBaselineHoldMs = 5000;

struct SoundResult {
    bool hold_expired = false;
    bool qualified = false;
    bool rearmed = false;
    bool rejected = false;
    bool activity = false;
    unsigned expired = 0;
    unsigned count = 0;
    unsigned remaining_ms = 0;
};

class SoundEvents {
public:
    constexpr explicit SoundEvents(SoundSettings settings) : settings_(settings) {}

    constexpr SoundResult sample(unsigned level, unsigned duration_ms, uint64_t now_us)
    {
        SoundResult result;
        while (count_ > 0 && now_us >= events_[0] &&
               now_us - events_[0] > settings_.window_ms * 1000ULL) {
            for (unsigned i = 1; i < count_; ++i) events_[i - 1] = events_[i];
            --count_;
            ++result.expired;
        }
        if (armed_) {
            if (level >= settings_.wake) {
                above_ms_ += duration_ms;
                if (above_ms_ >= settings_.qualify_ms) {
                    result.qualified = true;
                    armed_ = false;
                    above_ms_ = 0;
                    below_ms_ = 0;
                    events_[count_++] = now_us;
                    result.count = count_;
                    result.remaining_ms = settings_.window_ms -
                        static_cast<unsigned>((now_us - events_[0]) / 1000);
                    if (count_ >= settings_.required) {
                        result.activity = true;
                        count_ = 0;
                    }
                }
            } else {
                result.rejected = above_ms_ != 0;
                above_ms_ = 0;
            }
        } else {
            if (held_ms_ < kMaxBaselineHoldMs) {
                held_ms_ += duration_ms;
                result.hold_expired = held_ms_ >= kMaxBaselineHoldMs;
            }
            if (level < settings_.rearm) {
                below_ms_ += duration_ms;
                if (below_ms_ >= settings_.rearm_ms) {
                    armed_ = true;
                    below_ms_ = 0;
                    held_ms_ = 0;
                    result.rearmed = true;
                }
            } else {
                below_ms_ = 0;
            }
        }
        if (!result.qualified) result.count = count_;
        if (count_ > 0 && now_us >= events_[0]) {
            result.remaining_ms = settings_.window_ms -
                                  static_cast<unsigned>((now_us - events_[0]) / 1000);
        }
        return result;
    }

    constexpr void discontinuity()
    {
        above_ms_ = 0;
        below_ms_ = 0;
    }

    constexpr bool protect_baseline() const
    {
        return above_ms_ != 0 || (!armed_ && held_ms_ < kMaxBaselineHoldMs);
    }
    constexpr bool armed() const { return armed_; }
    constexpr unsigned above_ms() const { return above_ms_; }
    constexpr unsigned below_ms() const { return below_ms_; }
    constexpr unsigned held_ms() const { return held_ms_; }

private:
    SoundSettings settings_;
    std::array<uint64_t, 10> events_{};
    unsigned count_ = 0;
    unsigned above_ms_ = 0;
    unsigned below_ms_ = 0;
    unsigned held_ms_ = 0;
    bool armed_ = true;
};

class AmbientBaseline {
public:
    // One 16-bit LSB RMS; a real microphone noise floor is never below quantization.
    static constexpr float kFloor = 1.0F / 32768.0F;

    constexpr void observe(float rms, bool protected_event)
    {
        // All-zero windows (codec start-up, stalled capture) carry no ambient information.
        if (rms <= 0.0F) return;
        if (!initialized_) {
            value_ = rms > kFloor ? rms : kFloor;
            initialized_ = true;
        } else if (protected_event) {
            elevated_windows_ = 0;
        } else if (rms < value_) {
            elevated_windows_ = 0;
            value_ += (rms - value_) * 0.04F;
        } else if (rms < value_ * 3.0F) {
            elevated_windows_ = 0;
            value_ += (rms - value_) * 0.002F;
        } else {
            if (elevated_windows_ < 100) ++elevated_windows_;
            if (elevated_windows_ >= 100) value_ += (rms - value_) * 0.002F;
        }
        if (value_ < kFloor) value_ = kFloor;
    }

    constexpr float value() const { return value_; }
    constexpr bool initialized() const { return initialized_; }

private:
    float value_ = kFloor;
    unsigned elevated_windows_ = 0;
    bool initialized_ = false;
};
}
