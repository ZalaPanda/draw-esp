#include "display/live_brightness.h"

constexpr bool test_idle()
{
    display::LiveBrightness live;
    return !live.overrides() && live.take_target(80) == 0;
}

constexpr bool test_live_follow_and_commit()
{
    display::LiveBrightness live;
    live.preview(true, 80, 0);
    if (!live.overrides() || live.take_target(80) != 80) return false;
    live.preview(true, 80, 15);
    if (live.take_target(80) != 95) return false;
    live.preview(true, 80, 50);
    if (live.take_target(80) != 100) return false;
    live.preview(true, 80, -30);
    if (live.take_target(80) != 50) return false;
    live.request_commit(80, -30);
    live.preview(false, 80, 0);
    // No revert flicker between release and the display task saving the level.
    if (!live.overrides() || live.take_target(80) != 50) return false;
    live.finish_commit(true);
    return !live.overrides() && live.take_target(50) == 0;
}

constexpr bool test_cancel_reverts_once()
{
    display::LiveBrightness live;
    live.preview(true, 40, -100);
    if (live.take_target(40) != 10) return false;
    live.preview(false, 40, 0);
    if (live.overrides() || live.take_target(40) != 40) return false;
    live.preview(false, 40, 0);
    return live.take_target(40) == 0;
}

constexpr bool test_failed_commit_reverts()
{
    display::LiveBrightness live;
    live.preview(true, 60, 20);
    live.request_commit(60, 20);
    live.preview(false, 60, 0);
    live.finish_commit(false);
    return !live.overrides() && live.take_target(60) == 60 && live.take_target(60) == 0;
}

constexpr bool test_new_preview_clears_revert()
{
    display::LiveBrightness live;
    live.preview(true, 30, 10);
    live.preview(false, 30, 0);
    live.preview(true, 30, -5);
    return live.take_target(30) == 25;
}

static_assert(test_idle());
static_assert(test_live_follow_and_commit());
static_assert(test_cancel_reverts_once());
static_assert(test_failed_commit_reverts());
static_assert(test_new_preview_clears_revert());
