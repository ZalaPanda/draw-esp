#pragma once

#include <cstdint>

namespace display {
constexpr uint8_t brightness_register(unsigned percent)
{
    return static_cast<uint8_t>(percent * 255U / 100U);
}

constexpr int ramp_value(int start, int target, uint64_t elapsed_us, uint64_t duration_us)
{
    if (duration_us == 0 || elapsed_us >= duration_us) return target;
    return start + static_cast<int>((static_cast<int64_t>(target) - start) *
                                    static_cast<int64_t>(elapsed_us) /
                                    static_cast<int64_t>(duration_us));
}

struct ActivityState {
    unsigned active_brightness;
    uint64_t idle_us;
    uint64_t ramp_us;
    uint64_t deadline_us = 0;
    uint64_t started_us = 0;
    unsigned generation = 0;
    int start = 20;
    int target = 20;
    bool active = true;
    bool ramping = false;
    const char *reason = "startup";

    constexpr bool wake(uint64_t now, int applied, const char *activity_reason)
    {
        deadline_us = now + idle_us;
        if (active && ramping && target == static_cast<int>(active_brightness)) return false;
        begin(now, applied, static_cast<int>(active_brightness), true, activity_reason);
        return true;
    }

    constexpr bool expire(uint64_t now, int applied)
    {
        if (!active || now < deadline_us) return false;
        begin(now, applied, 0, false, "idle timeout");
        return true;
    }

    constexpr int value(uint64_t now) const
    {
        return ramp_value(start, target, now >= started_us ? now - started_us : 0, ramp_us);
    }

    constexpr bool finished(uint64_t now) const
    {
        return now >= started_us && now - started_us >= ramp_us;
    }

private:
    constexpr void begin(uint64_t now, int applied, int requested, bool on, const char *why)
    {
        ++generation;
        started_us = now;
        start = applied;
        target = requested;
        active = on;
        reason = why;
        ramping = true;
    }
};
}
