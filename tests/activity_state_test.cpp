#include "display/activity_state.h"

constexpr bool test_ramps()
{
    for (int start = 0; start <= 100; ++start) {
        for (int target = 0; target <= 100; ++target) {
            int previous = start;
            for (unsigned step = 0; step <= 20; ++step) {
                const int value = display::ramp_value(start, target, step * 20000, 400000);
                if (value < 0 || value > 100) return false;
                if (target >= start && value < previous) return false;
                if (target <= start && value > previous) return false;
                previous = value;
            }
            if (previous != target) return false;
        }
    }
    return true;
}

constexpr bool test_deadline_and_interruption()
{
    display::ActivityState state{80, 30000000, 400000};
    state.wake(226640001, 0, "sound");
    if (state.expire(226640000, 0)) return false;
    if (state.value(226640000) != 0) return false;
    if (state.expire(256640000, 80)) return false;
    if (!state.expire(256640001, 80)) return false;
    if (state.active || state.target != 0) return false;
    const int halfway = state.value(256840001);
    if (halfway != 40) return false;
    const unsigned cancelled = state.generation;
    state.wake(256840001, halfway, "rotation");
    if (state.generation != cancelled + 1 || !state.active) return false;
    if (state.value(256840001) != 40 || state.value(257040001) != 60) return false;
    if (state.finished(257240000) || !state.finished(257240001)) return false;
    if (state.value(257240001) != 80) return false;
    if (state.expire(286840000, 80)) return false;
    return state.expire(286840001, 80) && !state.expire(286840002, 80);
}

constexpr bool test_activity_sources()
{
    constexpr const char *sources[]{"image upload", "orientation change", "image clear",
                                  "sound bar shown", "sound bar hidden", "qualified sound events"};
    display::ActivityState state{80, 30000000, 400000};
    uint64_t now = 1000000;
    for (const char *source : sources) {
        state.wake(now, 0, source);
        if (!state.active || state.target != 80 || state.reason != source) return false;
        if (state.deadline_us != now + 30000000 || state.value(now + 400000) != 80) return false;
        if (!state.expire(now + 30000000, 80)) return false;
        if (state.value(now + 30400000) != 0) return false;
        now += 31000000;
    }
    return true;
}

constexpr bool test_repeated_activity_does_not_restart_ramp()
{
    display::ActivityState state{80, 30000000, 400000};
    state.wake(1000000, 0, "sound");
    const unsigned generation = state.generation;
    for (unsigned step = 1; step <= 20; ++step) {
        const uint64_t now = 1000000 + step * 20000;
        if (state.wake(now, state.value(now), "button")) return false;
        if (state.generation != generation || state.started_us != 1000000) return false;
    }
    return state.value(1400000) == 80 && state.finished(1400000) &&
           state.deadline_us == 31400000;
}

static_assert(test_ramps());
static_assert(test_deadline_and_interruption());
static_assert(test_activity_sources());
static_assert(test_repeated_activity_does_not_restart_ramp());
constexpr bool test_selected_brightness_retargets()
{
    display::ActivityState state{80, 30000000, 400000};
    state.wake(1000000, 0, "touch");
    const int halfway = state.value(1200000);
    if (halfway != 40) return false;
    state.active_brightness = 30;
    if (!state.wake(1200000, halfway, "brightness swipe")) return false;
    if (state.target != 30 || state.value(1200000) != 40 ||
        state.value(1400000) != 35 || state.value(1600000) != 30) return false;
    if (state.wake(1400000, 35, "sound")) return false;
    if (state.deadline_us != 31400000) return false;
    if (!state.expire(31400000, 30)) return false;
    const int fading = state.value(31600000);
    if (fading != 15) return false;
    state.active_brightness = 10;
    if (!state.wake(31600000, fading, "touch")) return false;
    if (state.value(31600000) != 15 || state.value(32000000) != 10) return false;
    return state.expire(61600000, 10) && state.value(62000000) == 0;
}

constexpr bool test_restored_targets()
{
    for (unsigned selected = 10; selected <= 100; selected += 10) {
        display::ActivityState state{selected, 30000000, 400000};
        state.start = state.target = static_cast<int>(selected);
        if (state.value(0) != static_cast<int>(selected)) return false;
        if (!state.wake(1000000, 0, "touch")) return false;
        if (state.value(1400000) != static_cast<int>(selected)) return false;
    }
    return true;
}

static_assert(test_selected_brightness_retargets());
static_assert(test_restored_targets());
static_assert(display::ramp_value(80, 0, 0, 0) == 0);
static_assert(display::ramp_value(80, 0, 1, 1) == 0);
static_assert(display::ramp_value(80, 0, 200000, 400000) == 40);
static_assert(display::ramp_value(80, 0, 400000, 400000) == 0);
static_assert(display::ramp_value(0, 80, 20, 401) == 3);
static_assert(display::ramp_value(0, 80, 401, 401) == 80);
static_assert(display::brightness_register(0) == 0);
static_assert(display::brightness_register(20) == 0x33);
static_assert(display::brightness_register(80) == 0xCC);
static_assert(display::brightness_register(100) == 0xFF);
