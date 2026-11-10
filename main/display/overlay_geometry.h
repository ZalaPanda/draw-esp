#pragma once

namespace display {
constexpr int kOverlaySize = 480;
constexpr int kBarWidth = 20;
constexpr int kBarMargin = 10;
constexpr int kBarHeight = kOverlaySize - 2 * kBarMargin;
constexpr int kSoundBarLeft = kBarMargin;
constexpr int kBrightnessBarLeft = kOverlaySize - kBarMargin - kBarWidth;

struct OverlayPoint {
    int x;
    int y;
};

// Exclusive end coordinates in physical panel pixels.
struct OverlayRect {
    int x0;
    int y0;
    int x1;
    int y1;
    constexpr int width() const { return x1 - x0; }
    constexpr int height() const { return y1 - y0; }
};

// Physical half-open interval along a bar's fill axis (x for 90/270, y for 0/180).
struct OverlaySpan {
    int start;
    int end;
    constexpr bool contains(int value) const { return value >= start && value < end; }
};

// Viewer coordinates for a physical pixel; matches render_source and touch logical_touch.
constexpr OverlayPoint viewer_from_physical(OverlayPoint p, unsigned rotation)
{
    switch (rotation) {
    case 1: return {p.y, kOverlaySize - 1 - p.x};
    case 2: return {kOverlaySize - 1 - p.x, kOverlaySize - 1 - p.y};
    case 3: return {kOverlaySize - 1 - p.y, p.x};
    default: return p;
    }
}

constexpr OverlayPoint physical_from_viewer(OverlayPoint v, unsigned rotation)
{
    switch (rotation) {
    case 1: return {kOverlaySize - 1 - v.y, v.x};
    case 2: return {kOverlaySize - 1 - v.x, kOverlaySize - 1 - v.y};
    case 3: return {v.y, kOverlaySize - 1 - v.x};
    default: return v;
    }
}

constexpr int min_int(int a, int b) { return a < b ? a : b; }
constexpr int max_int(int a, int b) { return a > b ? a : b; }

// Physical rectangle covered by a vertical bar whose viewer-frame left edge is viewer_left.
constexpr OverlayRect bar_rect(int viewer_left, unsigned rotation)
{
    const OverlayPoint a = physical_from_viewer({viewer_left, kBarMargin}, rotation);
    const OverlayPoint b = physical_from_viewer(
        {viewer_left + kBarWidth - 1, kBarMargin + kBarHeight - 1}, rotation);
    return {min_int(a.x, b.x), min_int(a.y, b.y), max_int(a.x, b.x) + 1, max_int(a.y, b.y) + 1};
}

constexpr int bar_filled_rows(unsigned percent)
{
    return static_cast<int>((static_cast<unsigned long long>(percent > 100 ? 100 : percent) *
                             kBarHeight) / 100);
}

// Filled physical span for a bar, computed once per update rather than per pixel.
constexpr OverlaySpan bar_fill_span(const OverlayRect &rect, unsigned rotation, unsigned percent)
{
    const int filled = bar_filled_rows(percent);
    switch (rotation) {
    case 1: return {rect.x0, rect.x0 + filled};
    case 2: return {rect.y0, rect.y0 + filled};
    case 3: return {rect.x1 - filled, rect.x1};
    default: return {rect.y1 - filled, rect.y1};
    }
}

// Whether a physical pixel inside the bar is filled; fill grows up from the viewer's bottom.
constexpr bool bar_pixel_filled(OverlayPoint physical, unsigned rotation, unsigned percent)
{
    const OverlayPoint v = viewer_from_physical(physical, rotation);
    return v.y >= kBarMargin + kBarHeight - bar_filled_rows(percent);
}
}
