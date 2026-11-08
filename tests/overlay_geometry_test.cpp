#include "display/overlay_geometry.h"
#include "input/touch_gesture.h"

#include <initializer_list>

using display::OverlayPoint;
using display::OverlayRect;

constexpr bool same(OverlayRect a, OverlayRect b)
{
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

constexpr bool test_inverse_and_touch_match()
{
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        for (int y = 0; y < 480; y += 7) {
            for (int x = 0; x < 480; x += 7) {
                const OverlayPoint v = display::viewer_from_physical({x, y}, rotation);
                const OverlayPoint p = display::physical_from_viewer(v, rotation);
                if (p.x != x || p.y != y) return false;
                const input::TouchPoint t = input::logical_touch({x, y}, rotation);
                if (t.x != v.x || t.y != v.y) return false;
            }
        }
    }
    return true;
}

// Sound bar on the viewer's left, brightness bar on the viewer's right, for every rotation.
static_assert(same(display::bar_rect(display::kSoundBarLeft, 0), {10, 10, 30, 470}));
static_assert(same(display::bar_rect(display::kBrightnessBarLeft, 0), {450, 10, 470, 470}));
static_assert(same(display::bar_rect(display::kSoundBarLeft, 1), {10, 10, 470, 30}));
static_assert(same(display::bar_rect(display::kBrightnessBarLeft, 1), {10, 450, 470, 470}));
static_assert(same(display::bar_rect(display::kSoundBarLeft, 2), {450, 10, 470, 470}));
static_assert(same(display::bar_rect(display::kBrightnessBarLeft, 2), {10, 10, 30, 470}));
static_assert(same(display::bar_rect(display::kSoundBarLeft, 3), {10, 450, 470, 470}));
static_assert(same(display::bar_rect(display::kBrightnessBarLeft, 3), {10, 10, 470, 30}));

constexpr bool test_rect_sizes_and_fill()
{
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        for (const int left : {display::kSoundBarLeft, display::kBrightnessBarLeft}) {
            const OverlayRect r = display::bar_rect(left, rotation);
            if (r.width() * r.height() != display::kBarWidth * display::kBarHeight) return false;
            for (const unsigned level : {0U, 10U, 50U, 100U}) {
                int filled = 0;
                for (int y = r.y0; y < r.y1; ++y) {
                    for (int x = r.x0; x < r.x1; ++x) {
                        filled += display::bar_pixel_filled({x, y}, rotation, level) ? 1 : 0;
                    }
                }
                if (filled != display::bar_filled_rows(level) * display::kBarWidth) return false;
            }
        }
    }
    return true;
}

// The fill grows from the viewer's bottom edge, whichever physical edge that is.
constexpr bool viewer_bottom_filled(unsigned rotation)
{
    const OverlayPoint bottom = display::physical_from_viewer({20, 469}, rotation);
    const OverlayPoint top = display::physical_from_viewer({20, 10}, rotation);
    return display::bar_pixel_filled(bottom, rotation, 10) &&
           !display::bar_pixel_filled(top, rotation, 10) &&
           display::bar_pixel_filled(top, rotation, 100);
}

static_assert(test_inverse_and_touch_match());
static_assert(test_rect_sizes_and_fill());
static_assert(viewer_bottom_filled(0) && viewer_bottom_filled(1) &&
              viewer_bottom_filled(2) && viewer_bottom_filled(3));
// Rotation 90: viewer bottom is the physical left edge.
static_assert(display::bar_pixel_filled({10, 20}, 1, 10) && !display::bar_pixel_filled({469, 20}, 1, 10));
