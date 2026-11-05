#include "input/button_debouncer.h"

constexpr bool test_press_release_and_hold()
{
    input::ButtonDebouncer button(false, 0, 50);
    return !button.sample(true, 10) &&
           !button.sample(true, 59) &&
           button.sample(true, 60) &&
           !button.sample(true, 5000) &&
           !button.sample(false, 5010) &&
           !button.sample(false, 5060) &&
           !button.sample(true, 5070) &&
           button.sample(true, 5120);
}

constexpr bool test_bounce_and_short_press()
{
    input::ButtonDebouncer button(false, 0, 50);
    return !button.sample(true, 10) &&
           !button.sample(false, 20) &&
           !button.sample(true, 30) &&
           !button.sample(true, 79) &&
           button.sample(true, 80) &&
           !button.sample(false, 90) &&
           !button.sample(true, 100) &&
           !button.sample(true, 150) &&
           !button.sample(false, 160) &&
           !button.sample(false, 210) &&
           !button.sample(true, 220) &&
           !button.sample(false, 240) &&
           !button.sample(false, 300);
}

constexpr bool test_held_at_start()
{
    input::ButtonDebouncer button(true, 0, 50);
    return !button.sample(true, 1000) &&
           !button.sample(false, 1010) &&
           !button.sample(false, 1060) &&
           !button.sample(true, 1070) &&
           button.sample(true, 1120);
}

constexpr bool test_tick_wraparound()
{
    input::ButtonDebouncer button(false, 0xFFFFFFE0u, 50);
    return !button.sample(true, 0xFFFFFFF0u) &&
           !button.sample(true, 0x21u) &&
           button.sample(true, 0x22u);
}

static_assert(test_press_release_and_hold());
static_assert(test_bounce_and_short_press());
static_assert(test_held_at_start());
static_assert(test_tick_wraparound());
