#pragma once

namespace display {
constexpr bool valid_brightness(unsigned value)
{
    return value >= 10 && value <= 100;
}

constexpr unsigned adjusted_brightness(unsigned value, int delta)
{
    const int next = static_cast<int>(value) + delta;
    return next < 10 ? 10U : next > 100 ? 100U : static_cast<unsigned>(next);
}
}
