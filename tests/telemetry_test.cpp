#include "system/cpu_load.h"

#include <cstdint>

constexpr bool test_cpu_load()
{
    using telemetry::cpu_load_percent;
    // Fully busy: no idle run time.
    if (cpu_load_percent(0, 30000000, 2) != 100) return false;
    // Fully idle on both cores.
    if (cpu_load_percent(60000000, 30000000, 2) != 0) return false;
    // Half of the available CPU time was idle.
    if (cpu_load_percent(30000000, 30000000, 2) != 50) return false;
    if (cpu_load_percent(15000000, 30000000, 2) != 75) return false;
    // Invalid window or core count.
    if (cpu_load_percent(12345, 0, 2) != 0) return false;
    if (cpu_load_percent(0, 0, 0) != 0) return false;
    // Idle time above the elapsed capacity clamps to 100% idle.
    if (cpu_load_percent(90000000, 30000000, 2) != 0) return false;
    // Single core boundaries.
    if (cpu_load_percent(0, 30000000, 1) != 100) return false;
    if (cpu_load_percent(30000000, 30000000, 1) != 0) return false;
    return true;
}

constexpr bool test_counter_delta()
{
    if (telemetry::counter_delta<uint32_t>(5, 0xFFFFFFF0u) != 21) return false;
    if (telemetry::counter_delta<uint32_t>(10, 10) != 0) return false;
    if (telemetry::counter_delta<uint32_t>(0, 0xFFFFFFFFu) != 1) return false;
    if (telemetry::counter_delta<uint64_t>(0, 0xFFFFFFFFFFFFFFFFull) != 1) return false;
    return true;
}

static_assert(test_cpu_load());
static_assert(test_counter_delta());
