#include "display/render_mapping.h"

#include <cstdint>

namespace {

// Original per-pixel mapping retained here as the reference implementation.
constexpr int legacy_index(int viewer_x, int viewer_y, int source_width, int source_height,
                           unsigned rotation)
{
    int sx = viewer_x;
    int sy = viewer_y;
    switch (rotation) {
    case 1: sx = viewer_y; sy = source_height - 1 - viewer_x; break;
    case 2: sx = source_width - 1 - viewer_x; sy = source_height - 1 - viewer_y; break;
    case 3: sx = source_width - 1 - viewer_y; sy = viewer_x; break;
    default: break;
    }
    return sy * source_width + sx;
}

constexpr bool test_rotation_mapping()
{
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        for (int source_height = 1; source_height <= 12; ++source_height) {
            for (int source_width = 1; source_width <= 12; ++source_width) {
                const bool quarter_turn = rotation == 1 || rotation == 3;
                const int viewer_width = quarter_turn ? source_height : source_width;
                const int viewer_height = quarter_turn ? source_width : source_height;
                const display::RotationTransform transform =
                    display::rotation_transform(rotation, source_width, source_height);
                for (int y = 0; y < viewer_height; ++y) {
                    for (int x = 0; x < viewer_width; ++x) {
                        if (display::source_index(transform, x, y) !=
                            legacy_index(x, y, source_width, source_height, rotation)) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

constexpr bool test_sampling()
{
    for (int offset = -480; offset <= 480; offset += 37) {
        for (int source_extent = 1; source_extent <= 480; source_extent += 53) {
            for (int scaled_extent = 1; scaled_extent <= 480; scaled_extent += 61) {
                for (int viewer = -240; viewer <= 720; viewer += 29) {
                    const int expected = static_cast<int>(
                        (static_cast<int64_t>(viewer - offset) * source_extent) / scaled_extent);
                    if (display::sample_coordinate(viewer, offset, source_extent, scaled_extent) !=
                        expected) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

} // namespace

static_assert(test_rotation_mapping());
static_assert(test_sampling());
