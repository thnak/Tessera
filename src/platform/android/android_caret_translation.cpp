// SPDX-License-Identifier: MIT

#include "android_caret_translation.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::hal::android {

namespace {

// A scale that cannot divide: the platform reports a density, and a zero would turn every point
// into NaN.
float safe_scale(float dpi_scale) {
    return dpi_scale > 0.0f ? dpi_scale : 1.0f;
}

} // namespace

CaretTranslation caret_from_point(const CaretField& field, const CaretSample& sample) {
    CaretTranslation result;
    if (field.style == nullptr || field.metrics == nullptr) {
        return result;
    }
    const float scale = safe_scale(sample.dpi_scale);
    // Physical pixels to the logical pixels layout produced. The engine's bounds are logical, so
    // this is the one conversion that makes a tap land on the glyph under the finger.
    const float local_x = sample.x / scale - field.box_x;
    const float local_y = sample.y / scale - field.box_y;

    // A tap outside the border box is not a tap on the field. The padding counts as inside: a
    // finger a few pixels off the text is still aiming at the field, and every platform clamps.
    if (local_x < 0.0f || local_y < 0.0f || local_x > field.box_width ||
        local_y > field.box_height) {
        return result;
    }

    // The text starts at the content box, so the point is measured from there. A point in the
    // padding becomes a negative coordinate, which `hit_test_point` clamps to the nearest caret -
    // that is the behaviour a tap on the left padding should have.
    const float content_x = local_x - field.pad_left;
    const float content_y = local_y - field.pad_top;
    const float content_width = std::max(0.0f, field.box_width - field.pad_left - field.pad_right);

    ui::text::TextConstraints constraints;
    constraints.wrap = field.wrap;
    constraints.max_width = content_width;

    result.hit =
        ui::text::hit_test_point(field.text, *field.style, constraints, content_x, content_y,
                                 *field.metrics, field.advance, field.advance_user);
    result.inside = true;
    return result;
}

} // namespace tessera::hal::android
