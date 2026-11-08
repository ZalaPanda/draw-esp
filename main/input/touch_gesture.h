#pragma once

#include <cstdint>

namespace input {
struct TouchPoint {
    int x;
    int y;
};

// Inverse of the display's software image rotation, after BSP axis mapping.
constexpr TouchPoint logical_touch(TouchPoint point, unsigned rotation)
{
    switch (rotation) {
    case 1: return {point.y, 479 - point.x};
    case 2: return {479 - point.x, 479 - point.y};
    case 3: return {479 - point.y, point.x};
    default: return point;
    }
}

constexpr int swipe_delta(TouchPoint first, TouchPoint last)
{
    const int dx = last.x - first.x;
    const int dy = last.y - first.y;
    const int ax = dx < 0 ? -dx : dx;
    const int ay = dy < 0 ? -dy : dy;
    if (ay < 20 || ay <= ax) return 0;
    const int points = (ay * 100 + 240) / 480;
    // Board testing confirms mapped Y increases for a physical upward swipe.
    return dy > 0 ? points : -points;
}

struct TouchReport {
    unsigned points = 0;
    TouchPoint point = {};
};

constexpr bool decode_touch_report(const uint8_t *data, TouchReport &report)
{
    if (data[6] != 0xAB) return false;
    report.points = data[5] & 0x7F;
    if (report.points != 1) return true;
    const unsigned status = data[0] & 0x0F;
    if (status == 0) {
        report.points = 0;
        return true;
    }
    if (status != 6) return false;
    const int x = (data[1] << 4) | (data[3] >> 4);
    const int y = (data[2] << 4) | (data[3] & 0x0F);
    if (x >= 480 || y >= 480) return false;
    report.point = {y, 479 - x};
    return true;
}

class TouchGesture {
public:
    constexpr bool needs_begin(unsigned points) const
    {
        return points != 0 && !touching_ && !blocked_;
    }

    constexpr void begin(TouchPoint point, unsigned rotation, unsigned generation, bool wake_only)
    {
        touching_ = true;
        blocked_ = wake_only;
        released_ = 0;
        rotation_ = rotation;
        generation_ = generation;
        first_ = last_ = logical_touch(point, rotation_);
    }

    constexpr void cancel()
    {
        touching_ = false;
        blocked_ = true;
        released_ = 0;
    }

    constexpr int sample(unsigned points, TouchPoint point, unsigned generation)
    {
        if (touching_ && generation != generation_) cancel();
        if (points != 0) {
            released_ = 0;
            if (points != 1 || !touching_) {
                cancel();
            } else {
                last_ = logical_touch(point, rotation_);
            }
            return 0;
        }
        if (released_ < 3) ++released_;
        if (released_ != 3) return 0;
        const int delta = touching_ && !blocked_ ? swipe_delta(first_, last_) : 0;
        touching_ = false;
        blocked_ = false;
        return delta;
    }

    constexpr unsigned generation() const { return generation_; }
    constexpr bool touching() const { return touching_; }
    // True for a non-wake, uncancelled contact, including its release debounce.
    constexpr bool adjusting() const { return touching_ && !blocked_; }
    constexpr int preview_delta() const
    {
        return touching_ && !blocked_ ? swipe_delta(first_, last_) : 0;
    }

private:
    bool touching_ = false;
    bool blocked_ = true;
    unsigned released_ = 0;
    unsigned rotation_ = 0;
    unsigned generation_ = 0;
    TouchPoint first_ = {};
    TouchPoint last_ = {};
};
}
