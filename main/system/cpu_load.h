#pragma once

#include <cstdint>

namespace telemetry {

// Aggregate CPU load in percent across `core_count` cores from idle-task run
// time counters. `idle_delta_us` is the wrap-safe sum of the idle counters'
// deltas (microseconds) over the same wall-clock window as `elapsed_us`.
constexpr unsigned cpu_load_percent(uint64_t idle_delta_us, uint64_t elapsed_us, unsigned core_count)
{
    if (elapsed_us == 0 || core_count == 0) return 0;
    const uint64_t capacity_us = elapsed_us * core_count;
    const uint64_t idle_us = idle_delta_us > capacity_us ? capacity_us : idle_delta_us;
    const unsigned idle_percent = static_cast<unsigned>(idle_us * 100 / capacity_us);
    return idle_percent >= 100 ? 0 : 100 - idle_percent;
}

// Unsigned subtraction is defined modulo the counter width, so wrapping run
// time counters still produce the correct delta.
template <typename Counter>
constexpr uint64_t counter_delta(Counter current, Counter previous)
{
    return static_cast<uint64_t>(static_cast<Counter>(current - previous));
}

} // namespace telemetry
