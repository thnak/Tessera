// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/hit_test.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <string_view>

// Pure point-to-caret translation for the Android shell (issue #189, docs/guides/android-lane.md
// section 3.13).
//
// A custom-rendered text field does not get caret placement for free: the platform reports a touch
// as a point in view coordinates, and the engine's edit controller wants a `TextHit` (a UTF-8 byte
// offset plus a caret affinity). `hit_test_point` already does the hard part - line selection by y,
// then x inside that line's visual runs, bidi-aware and snapped to a grapheme boundary - but it
// takes a `TextStyle`, a `TextConstraints` and a `FontMetrics`, and the bridge has none of those.
//
// This header is the missing adapter, and it is PURE: no NDK, no JNI, no device. The box arithmetic
// (physical pixels to the content box, the padding, the density scale) is where a caret lands one
// character off, so it is unit-tested on every lane instead of only on a phone.
//
// The split mirrors the touch path (`android_motion_translation.hpp`): the bridge reads the Java
// getters into a plain struct, and this header does the mapping.

namespace tessera::hal::android {

// The field's painted box and text facts, in the units the engine's layout produced (logical
// pixels). The bridge fills this from the node's bounds and its computed style; a test fills it
// directly.
struct CaretField {
    // The node's border box, in logical pixels, relative to the window's content origin.
    float box_x = 0.0f;
    float box_y = 0.0f;
    float box_width = 0.0f;
    float box_height = 0.0f;
    // The node's padding, in logical pixels. The text starts inside it, so a point in the padding
    // must clamp to the nearest caret rather than resolve to a negative offset.
    float pad_left = 0.0f;
    float pad_top = 0.0f;
    float pad_right = 0.0f;
    float pad_bottom = 0.0f;
    // The text the field currently paints. Borrowed: it must outlive the call.
    std::string_view text;
    // The style the field paints with, and the metrics its layout used. Both come from the same
    // sources paint reads, so a caret lands where the glyphs are.
    const ui::text::TextStyle* style = nullptr;
    const ui::text::FontMetrics* metrics = nullptr;
    // The advance seam paint measures with, and its context. Null falls back to the deterministic
    // stub, whose advances do not match a real face: a tap in the middle of a word then resolves to
    // the end of the line. The shell passes the same source paint uses.
    ui::text::TextAdvanceMeasure advance = nullptr;
    const void* advance_user = nullptr;
    // True when the field wraps at its content width, which is what paint does for a multi-line
    // field. A single-line field does not wrap, so a long value must not be broken into lines the
    // caret could land on.
    bool wrap = true;
};

// One touch, reduced to the values the mapping needs. `x`/`y` are in PHYSICAL pixels relative to
// the window's content origin, which is what a Java `MotionEvent` reports; `dpi_scale` converts
// them to the logical pixels the layout uses.
struct CaretSample {
    float x = 0.0f;
    float y = 0.0f;
    float dpi_scale = 1.0f;
};

// The caret a touch resolves to, or `byte_offset == 0` with `inside == false` when the point is
// outside the field's box entirely (a tap on the padding still counts as inside: it clamps to the
// nearest caret, which is what every platform does).
struct CaretTranslation {
    ui::text::TextHit hit{};
    // False when the point missed the field, so the caller can leave the caret alone instead of
    // jumping it to the start.
    bool inside = false;
};

// Maps one touch to a caret. A point outside the field's border box is reported as `inside ==
// false`; a point inside it - including in the padding - resolves to the nearest caret, because a
// tap that lands a few pixels off the text is still a tap on the field.
[[nodiscard]] CaretTranslation caret_from_point(const CaretField& field, const CaretSample& sample);

} // namespace tessera::hal::android
