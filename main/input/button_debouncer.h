#pragma once

#include <cstdint>

namespace input {
class ButtonDebouncer {
public:
    constexpr ButtonDebouncer(bool pressed, uint32_t now, uint32_t debounce_ticks)
        : candidate_(pressed), stable_(pressed), changed_at_(now), debounce_ticks_(debounce_ticks)
    {
    }

    constexpr bool sample(bool pressed, uint32_t now)
    {
        bool stable_state = false;
        return sample(pressed, now, stable_state) && stable_state;
    }

    constexpr bool sample(bool pressed, uint32_t now, bool &new_stable_state)
    {
        if (pressed != candidate_) {
            candidate_ = pressed;
            changed_at_ = now;
        }
        if (candidate_ != stable_ && uint32_t(now - changed_at_) >= debounce_ticks_) {
            stable_ = candidate_;
            new_stable_state = stable_;
            return true;
        }
        new_stable_state = stable_;
        return false;
    }

private:
    bool candidate_;
    bool stable_;
    uint32_t changed_at_;
    uint32_t debounce_ticks_;
};
}
