#pragma once

#include <cstdint>

namespace display {

// Maps viewer coordinates to the unrotated source image. The viewer frame has
// the rotated dimensions (width/height are swapped for 90 and 270 degrees).
struct RotationTransform {
    int x_step;
    int y_step;
    int base;
};

// viewer_x counts across the rotated width and viewer_y across the rotated
// height. The result is an index into the unrotated source image.
constexpr RotationTransform rotation_transform(unsigned rotation, int source_width, int source_height)
{
    switch (rotation) {
    case 1: return {-source_width, 1, (source_height - 1) * source_width};
    case 2: return {-1, -source_width, (source_height - 1) * source_width + source_width - 1};
    case 3: return {source_width, -1, source_width - 1};
    default: return {1, source_width, 0};
    }
}

constexpr int source_index(RotationTransform transform, int viewer_x, int viewer_y)
{
    return transform.base + viewer_y * transform.y_step + viewer_x * transform.x_step;
}

// Nearest-neighbor sample coordinate used by every fit mode. The integer
// division intentionally truncates toward zero, matching the previous inline
// expressions (including negative off-image coordinates).
constexpr int sample_coordinate(int viewer_coordinate, int offset, int source_extent, int scaled_extent)
{
    return static_cast<int>((static_cast<int64_t>(viewer_coordinate - offset) * source_extent) /
                            scaled_extent);
}

} // namespace display
