#include "input/touch_gesture.h"
#include "display/brightness_level.h"

constexpr bool release(input::TouchGesture &gesture, unsigned generation, int expected)
{
    if (gesture.sample(0, {}, generation) != 0) return false;
    if (gesture.sample(0, {}, generation) != 0) return false;
    if (gesture.sample(0, {}, generation) != expected) return false;
    return gesture.sample(0, {}, generation) == 0;
}

constexpr bool test_rotations()
{
    constexpr input::TouchPoint ends_up[]{{240, 300}, {180, 240}, {240, 180}, {300, 240}};
    constexpr input::TouchPoint ends_down[]{{240, 180}, {300, 240}, {240, 300}, {180, 240}};
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        for (int direction = 0; direction < 2; ++direction) {
            input::TouchGesture gesture;
            if (!release(gesture, 7, 0)) return false;
            if (!gesture.needs_begin(1)) return false;
            gesture.begin({240, 240}, rotation, 7, false);
            const auto end = direction == 0 ? ends_up[rotation] : ends_down[rotation];
            if (gesture.sample(1, end, 7) != 0) return false;
            if (!release(gesture, 7, direction == 0 ? 13 : -13)) return false;
        }
    }
    return true;
}

constexpr bool test_cancellation()
{
    input::TouchGesture gesture;
    if (gesture.needs_begin(1)) return false;
    if (gesture.sample(1, {240, 240}, 0) != 0) return false;
    if (!release(gesture, 0, 0)) return false;
    if (!gesture.needs_begin(2)) return false;
    gesture.begin({}, 0, 0, false);
    gesture.sample(2, {}, 0);
    if (!release(gesture, 0, 0)) return false;
    gesture.begin({240, 240}, 0, 0, true);
    gesture.sample(1, {240, 100}, 0);
    if (!release(gesture, 0, 0)) return false;
    gesture.begin({240, 240}, 0, 0, false);
    gesture.sample(1, {240, 100}, 0);
    gesture.sample(2, {240, 100}, 0);
    gesture.sample(1, {240, 100}, 0);
    if (!release(gesture, 0, 0)) return false;
    gesture.begin({240, 240}, 0, 0, false);
    gesture.sample(1, {240, 100}, 1);
    if (!release(gesture, 1, 0)) return false;
    gesture.begin({240, 240}, 0, 1, false);
    gesture.sample(1, {240, 100}, 1);
    gesture.cancel();
    gesture.sample(1, {240, 100}, 1);
    if (!release(gesture, 1, 0)) return false;
    gesture.begin({240, 240}, 0, 1, false);
    gesture.sample(1, {240, 100}, 1);
    return release(gesture, 2, 0);
}

constexpr bool test_hold_and_release_bounce()
{
    input::TouchGesture gesture;
    release(gesture, 0, 0);
    gesture.begin({240, 240}, 0, 0, false);
    for (unsigned i = 0; i < 100; ++i) {
        if (gesture.sample(1, {240, 240}, 0) != 0) return false;
    }
    if (!release(gesture, 0, 0)) return false;
    gesture.begin({240, 240}, 0, 0, false);
    gesture.sample(1, {240, 100}, 0);
    if (gesture.sample(0, {}, 0) != 0) return false;
    gesture.sample(1, {240, 100}, 0);
    return release(gesture, 0, -29) && gesture.needs_begin(1);
}

constexpr bool test_reports()
{
    input::TouchReport report;
    uint8_t data[10] = {};
    if (input::decode_touch_report(data, report)) return false;
    data[6] = 0xAB;
    if (!input::decode_touch_report(data, report) || report.points != 0) return false;
    data[5] = 2;
    if (!input::decode_touch_report(data, report) || report.points != 2) return false;
    data[5] = 1;
    data[0] = 6;
    data[1] = 10;
    data[2] = 20;
    data[3] = 0x5A;
    if (!input::decode_touch_report(data, report) ||
        report.point.x != 330 || report.point.y != 314) return false;
    data[1] = 30;
    if (input::decode_touch_report(data, report)) return false;
    data[1] = 0;
    data[2] = 30;
    if (input::decode_touch_report(data, report)) return false;
    data[0] = 7;
    if (input::decode_touch_report(data, report)) return false;
    data[0] = 0;
    return input::decode_touch_report(data, report) && report.points == 0;
}

constexpr bool test_limits()
{
    unsigned percent = 80;
    for (unsigned i = 0; i < 20; ++i) {
        percent = display::adjusted_brightness(percent, -10);
        if (!display::valid_brightness(percent)) return false;
    }
    if (percent != 10) return false;
    for (unsigned i = 0; i < 20; ++i) percent = display::adjusted_brightness(percent, 10);
    return percent == 100 && display::adjusted_brightness(100, 10) == 100;
}

static_assert(test_rotations());
static_assert(test_cancellation());
static_assert(test_hold_and_release_bounce());
static_assert(test_reports());
static_assert(test_limits());
static_assert(input::swipe_delta({240, 240}, {240, 221}) == 0);
static_assert(input::swipe_delta({240, 240}, {240, 220}) == -4);
static_assert(input::swipe_delta({240, 240}, {240, 259}) == 0);
static_assert(input::swipe_delta({240, 240}, {240, 260}) == 4);
static_assert(input::swipe_delta({240, 240}, {240, 200}) == -8);
static_assert(input::swipe_delta({240, 240}, {300, 180}) == 0);
static_assert(input::swipe_delta({240, 240}, {340, 230}) == 0);
static_assert(input::swipe_delta({0, 0}, {0, 240}) == 50);
static_assert(input::swipe_delta({0, 240}, {0, 0}) == -50);
static_assert(input::swipe_delta({0, 0}, {0, 480}) == 100);
static_assert(input::swipe_delta({0, 0}, {0, 479}) == 100);
static_assert(input::swipe_delta({0, 0}, {0, 432}) == 90);
static_assert(display::adjusted_brightness(
    display::adjusted_brightness(10, input::swipe_delta({0, 0}, {0, 240})),
    input::swipe_delta({0, 0}, {0, 240})) == 100);

constexpr bool test_proportional_release()
{
    input::TouchGesture gesture;
    release(gesture, 0, 0);
    gesture.begin({240, 0}, 0, 0, false);
    for (int y = 10; y <= 240; y += 10) {
        if (gesture.sample(1, {240, y}, 0) != 0) return false;
    }
    if (!release(gesture, 0, 50)) return false;
    unsigned percent = display::adjusted_brightness(10, 50);
    gesture.begin({240, 0}, 0, 0, false);
    if (gesture.sample(1, {240, 240}, 0) != 0) return false;
    if (gesture.sample(0, {}, 0) != 0 || gesture.sample(0, {}, 0) != 0) return false;
    percent = display::adjusted_brightness(percent, gesture.sample(0, {}, 0));
    if (percent != 100 || gesture.sample(0, {}, 0) != 0) return false;
    gesture.begin({240, 240}, 0, 0, false);
    gesture.sample(1, {240, 200}, 0);
    gesture.sample(1, {240, 225}, 0);
    return release(gesture, 0, 0);
}

static_assert(test_proportional_release());

constexpr bool test_distance_scale()
{
    int previous = 0;
    for (int pixels = 0; pixels < 480; ++pixels) {
        const int up = input::swipe_delta({0, 0}, {0, pixels});
        const int down = input::swipe_delta({0, pixels}, {0, 0});
        if (up != -down || up < previous || up > 100) return false;
        if (pixels < 20 && up != 0) return false;
        if (pixels >= 20 && up != (pixels * 100 + 240) / 480) return false;
        previous = up;
    }
    return previous == 100;
}

static_assert(test_distance_scale());

constexpr bool test_brightness_preview()
{
    input::TouchGesture gesture;
    if (gesture.touching() || gesture.adjusting() || gesture.preview_delta() != 0) return false;
    release(gesture, 0, 0);
    gesture.begin({240, 0}, 0, 0, false);
    if (!gesture.adjusting() || gesture.preview_delta() != 0) return false;
    gesture.sample(1, {240, 19}, 0);
    if (gesture.preview_delta() != 0) return false;
    gesture.sample(1, {240, 20}, 0);
    if (gesture.preview_delta() != 4) return false;
    gesture.sample(1, {240, 240}, 0);
    if (gesture.preview_delta() != 50 ||
        display::adjusted_brightness(80, gesture.preview_delta()) != 100) return false;
    // The preview stays live through the release debounce, then the delta is committed.
    if (gesture.sample(0, {}, 0) != 0 || gesture.sample(0, {}, 0) != 0) return false;
    if (!gesture.adjusting() || gesture.preview_delta() != 50) return false;
    if (gesture.sample(0, {}, 0) != 50 || gesture.adjusting()) return false;
    if (gesture.touching() || gesture.preview_delta() != 0) return false;
    gesture.begin({240, 240}, 0, 0, true);
    gesture.sample(1, {240, 400}, 0);
    if (!gesture.touching() || gesture.adjusting() || gesture.preview_delta() != 0) return false;
    if (!release(gesture, 0, 0)) return false;
    gesture.begin({240, 240}, 0, 0, false);
    gesture.sample(1, {240, 0}, 0);
    if (gesture.preview_delta() != -50 ||
        display::adjusted_brightness(30, gesture.preview_delta()) != 10) return false;
    gesture.sample(2, {}, 0);
    if (gesture.adjusting() || gesture.preview_delta() != 0) return false;
    release(gesture, 0, 0);
    gesture.begin({240, 0}, 0, 0, false);
    gesture.sample(1, {240, 240}, 1);
    if (gesture.adjusting() || gesture.preview_delta() != 0) return false;
    gesture.cancel();
    return !gesture.adjusting() && gesture.preview_delta() == 0;
}

static_assert(test_brightness_preview());
static_assert(!display::valid_brightness(0));
static_assert(!display::valid_brightness(9));
static_assert(display::valid_brightness(10));
static_assert(display::valid_brightness(100));
static_assert(!display::valid_brightness(101));
static_assert(display::adjusted_brightness(15, -10) == 10);
static_assert(display::adjusted_brightness(95, 10) == 100);
