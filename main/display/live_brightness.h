#pragma once

#include "brightness_level.h"

namespace display {
// Panel brightness while a swipe is in progress: applied immediately, saved only on release.
struct LiveBrightness {
    unsigned level = 0;
    bool adjusting = false;
    bool commit_pending = false;
    bool revert_pending = false;

    constexpr void preview(bool active, unsigned saved, int delta)
    {
        if (active) {
            adjusting = true;
            revert_pending = false;
            level = adjusted_brightness(saved, delta);
            return;
        }
        if (adjusting && !commit_pending) revert_pending = true;
        adjusting = false;
    }

    // Holds the released level until the display task has persisted it.
    constexpr void request_commit(unsigned saved, int delta)
    {
        commit_pending = true;
        revert_pending = false;
        level = adjusted_brightness(saved, delta);
    }

    constexpr void finish_commit(bool saved)
    {
        commit_pending = false;
        if (!saved && !adjusting) revert_pending = true;
    }

    constexpr bool overrides() const { return adjusting || commit_pending; }

    // Brightness to apply directly now, or 0 when the normal ramp logic owns the panel.
    constexpr unsigned take_target(unsigned saved)
    {
        if (overrides()) return level;
        if (!revert_pending) return 0;
        revert_pending = false;
        return saved;
    }
};
}
