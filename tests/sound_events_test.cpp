#include "input/sound_events.h"

constexpr input::SoundSettings defaults{60, 40, 150, 500, 2, 10000};

constexpr bool test_qualification_rearm_and_pair()
{
    input::SoundEvents events(defaults);
    uint64_t now = 0;
    for (unsigned i = 0; i < 7; ++i) {
        now += 20000;
        if (events.sample(60, 20, now).qualified) return false;
    }
    auto result = events.sample(60, 20, now += 20000);
    if (!result.qualified || result.activity || result.count != 1) return false;
    for (unsigned i = 0; i < 1000; ++i) {
        if (events.sample(100, 20, now += 20000).qualified) return false;
    }
    // The old event has expired, but sustained sound still cannot count twice.
    for (unsigned i = 0; i < 24; ++i) {
        if (events.sample(39, 20, now += 20000).rearmed) return false;
    }
    if (!events.sample(39, 20, now += 20000).rearmed) return false;
    for (unsigned i = 0; i < 8; ++i) result = events.sample(60, 20, now += 20000);
    if (!result.qualified || result.count != 1) return false;
    for (unsigned i = 0; i < 25; ++i) events.sample(0, 20, now += 20000);
    for (unsigned i = 0; i < 8; ++i) result = events.sample(60, 20, now += 20000);
    return result.qualified && result.activity && result.count == 2 && result.remaining_ms == 9340;
}

constexpr bool test_discontinuity_and_thresholds()
{
    input::SoundEvents events(defaults);
    for (unsigned i = 0; i < 7; ++i) events.sample(100, 20, i * 20000);
    events.discontinuity();
    if (events.above_ms() != 0) return false;
    if (events.sample(100, 20, 10000000).qualified) return false;
    auto result = events.sample(59, 20, 10020000);
    if (!result.rejected || events.protect_baseline()) return false;
    for (unsigned i = 0; i < 8; ++i) result = events.sample(60, 20, 10100000 + i * 20000);
    if (!result.qualified || !events.protect_baseline()) return false;
    for (unsigned i = 0; i < 24; ++i) events.sample(39, 20, 10300000 + i * 20000);
    events.discontinuity();
    if (events.sample(39, 20, 11000000).rearmed) return false;
    events.sample(40, 20, 11020000);
    if (events.below_ms() != 0) return false;
    for (unsigned i = 0; i < 25; ++i) result = events.sample(39, 20, 11100000 + i * 20000);
    return result.rearmed && events.armed();
}

constexpr bool test_rolling_window()
{
    constexpr input::SoundSettings settings{60, 40, 20, 20, 3, 1000};
    input::SoundEvents events(settings);
    events.sample(60, 20, 0);
    events.sample(0, 20, 20000);
    events.sample(60, 20, 800000);
    events.sample(0, 20, 820000);
    auto result = events.sample(60, 20, 1100000);
    if (result.expired != 1 || result.count != 2 || result.activity) return false;
    events.sample(0, 20, 1120000);
    result = events.sample(60, 20, 1200000);
    return result.activity && result.count == 3;
}

constexpr bool test_exact_window()
{
    input::SoundEvents events({60, 40, 20, 20, 2, 1000});
    events.sample(60, 20, 0);
    events.sample(0, 20, 20000);
    if (!events.sample(60, 20, 1000000).activity) return false;
    events.sample(0, 20, 1020000);
    events.sample(60, 20, 1040000);
    const auto expired = events.sample(0, 20, 2040001);
    return expired.expired == 1 && expired.count == 0;
}

constexpr bool test_baseline_protection()
{
    input::AmbientBaseline baseline;
    baseline.observe(0.01F, false);
    const float initial = baseline.value();
    for (unsigned i = 0; i < 1000; ++i) baseline.observe(0.5F, true);
    if (baseline.value() != initial) return false;
    for (unsigned i = 0; i < 99; ++i) baseline.observe(0.1F, false);
    if (baseline.value() != initial) return false;
    baseline.observe(0.1F, false);
    return baseline.value() > initial;
}

constexpr bool test_quiet_and_short_impulses()
{
    input::SoundEvents events(defaults);
    uint64_t now = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        const auto result = events.sample(30, 20, now += 20000);
        if (result.qualified || result.count != 0 || !events.armed()) return false;
    }
    for (unsigned i = 0; i < 7; ++i) {
        if (events.sample(100, 20, now += 20000).qualified) return false;
    }
    const auto rejected = events.sample(0, 20, now += 20000);
    return rejected.rejected && rejected.count == 0 && !events.protect_baseline();
}

constexpr bool test_odd_durations_and_maximum_count()
{
    input::SoundEvents odd({60, 40, 151, 501, 2, 1000});
    uint64_t now = 0;
    input::SoundResult result;
    for (unsigned i = 0; i < 8; ++i) result = odd.sample(60, 20, now += 20000);
    if (!result.qualified) return false;
    for (unsigned i = 0; i < 25; ++i) {
        if (odd.sample(0, 20, now += 20000).rearmed) return false;
    }
    if (!odd.sample(0, 20, now += 20000).rearmed) return false;
    input::SoundEvents maximum({60, 40, 20, 20, 10, 1000});
    for (unsigned i = 1; i <= 10; ++i) {
        result = maximum.sample(60, 20, now += 20000);
        if (!result.qualified || result.count != i || result.activity != (i == 10)) return false;
        if (!maximum.sample(0, 20, now += 20000).rearmed) return false;
    }
    return true;
}

constexpr bool test_zero_windows_do_not_initialize_baseline()
{
    input::AmbientBaseline baseline;
    for (unsigned i = 0; i < 10; ++i) baseline.observe(0.0F, false);
    if (baseline.initialized()) return false;
    baseline.observe(1.0e-7F, false);
    if (!baseline.initialized() || baseline.value() != input::AmbientBaseline::kFloor) return false;
    baseline.observe(0.0F, false);
    return baseline.value() == input::AmbientBaseline::kFloor;
}

// Regression for the observed deadlock: baseline 0.000001, ambient RMS ~0.00015 held the
// level at 100%, so the detector stayed unarmed and froze the baseline permanently.
constexpr bool test_unarmed_hold_expires_and_recovers()
{
    input::SoundEvents events(defaults);
    input::AmbientBaseline baseline;
    baseline.observe(1.0e-7F, false);
    constexpr float kAmbient = 0.00015F;
    constexpr float kRearmRatio = 3.98F;
    uint64_t now = 0;
    for (unsigned i = 0; i < 8; ++i) events.sample(100, 20, now += 20000);
    if (events.armed() || !events.protect_baseline()) return false;
    unsigned expirations = 0;
    for (unsigned i = 0; i < 250; ++i) {
        const auto result = events.sample(100, 20, now += 20000);
        if (result.qualified) return false;
        if (result.hold_expired) ++expirations;
        baseline.observe(kAmbient, events.protect_baseline());
    }
    if (expirations != 1 || events.protect_baseline() || events.held_ms() != input::kMaxBaselineHoldMs)
        return false;
    if (baseline.value() != input::AmbientBaseline::kFloor) return false;
    unsigned windows = 0;
    while (baseline.value() * kRearmRatio < kAmbient) {
        if (++windows > 200) return false;
        if (events.sample(100, 20, now += 20000).hold_expired) return false;
        baseline.observe(kAmbient, events.protect_baseline());
    }
    input::SoundResult result;
    for (unsigned i = 0; i < 25; ++i) result = events.sample(39, 20, now += 20000);
    return result.rearmed && events.armed() && events.held_ms() == 0;
}

static_assert(defaults.valid());
static_assert(!input::SoundSettings{60, 60, 150, 500, 2, 10000}.valid());
static_assert(!input::SoundSettings{60, 0, 150, 500, 2, 10000}.valid());
static_assert(!input::SoundSettings{60, 40, 150, 500, 0, 10000}.valid());
static_assert(!input::SoundSettings{60, 40, 150, 500, 11, 10000}.valid());
static_assert(!input::SoundSettings{60, 40, 2000, 5000, 10, 1000}.valid());
static_assert(!input::SoundSettings{60, 40, 21, 21, 2, 60}.valid());
static_assert(input::SoundSettings{60, 40, 21, 21, 2, 80}.valid());
static_assert(test_qualification_rearm_and_pair());
static_assert(test_discontinuity_and_thresholds());
static_assert(test_rolling_window());
static_assert(test_exact_window());
static_assert(test_baseline_protection());
static_assert(test_quiet_and_short_impulses());
static_assert(test_odd_durations_and_maximum_count());
static_assert(test_zero_windows_do_not_initialize_baseline());
static_assert(test_unarmed_hold_expires_and_recovers());
