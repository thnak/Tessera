// SPDX-License-Identifier: MIT
//
// Point-to-caret translation for the Android shell (issue #189, docs/guides/android-lane.md
// section 3.13).
//
// A custom-rendered text field does not get caret placement for free: the platform reports a touch
// as a point in view coordinates, and the edit controller wants a `TextHit`. The box arithmetic
// between them - physical pixels to the content box, the padding, the density scale - is where a
// caret lands one character off, so it is pinned here rather than on a phone.
//
// The rules are pure over plain numbers, so they run on every lane with no NDK, no JNI and no
// device.

#include "android_caret_translation.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <iostream>
#include <string_view>

namespace {

namespace android = tessera::hal::android;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::TextStyle;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// A field at (100, 50), 200x48 logical, with 12 of padding on every side and the text "hello".
// The metrics are the stub's shape (ascent 0.8em, descent 0.2em at 16px), so a line is 16 tall.
android::CaretField field(std::string_view text) {
    android::CaretField result;
    result.box_x = 100.0f;
    result.box_y = 50.0f;
    result.box_width = 200.0f;
    result.box_height = 48.0f;
    result.pad_left = 12.0f;
    result.pad_top = 12.0f;
    result.pad_right = 12.0f;
    result.pad_bottom = 12.0f;
    result.text = text;
    result.wrap = false;
    return result;
}

} // namespace

int main() {
    const TextStyle style{};
    const FontMetrics metrics{12.8f, 3.2f, 0.0f};

    // --- The box is in WINDOW coordinates, not the parent's -----------------------------------
    //
    // A node's `bounds` are parent-relative; a touch is window-relative. Reading the wrong one put
    // the field 24 px left of where it paints on the device, so a tap in the middle of "hello"
    // resolved past the end of the line and a drag selected nothing. The mapping itself cannot tell
    // the difference - it trusts the box - so this pins the contract the caller must honour: the
    // same point inside the text resolves to the same caret whether the field sits at the origin or
    // is offset, and an un-offset point does NOT resolve as if the field were at the origin.
    {
        android::CaretField at_origin = field("hello");
        at_origin.box_x = 0.0f;
        at_origin.box_y = 0.0f;
        at_origin.style = &style;
        at_origin.metrics = &metrics;
        android::CaretField offset = at_origin;
        offset.box_x = 48.0f;
        offset.box_y = 174.0f;

        android::CaretSample origin_sample;
        origin_sample.x = 34.0f;
        origin_sample.y = 12.0f;
        android::CaretSample offset_sample;
        offset_sample.x = 34.0f + 48.0f;
        offset_sample.y = 12.0f + 174.0f;

        const android::CaretTranslation a = android::caret_from_point(at_origin, origin_sample);
        const android::CaretTranslation b = android::caret_from_point(offset, offset_sample);
        check(a.inside && b.inside, "both points are inside their field");
        check(a.hit.byte_offset == b.hit.byte_offset,
              "a window-relative box resolves the same caret at any offset");
        const android::CaretTranslation wrong = android::caret_from_point(offset, origin_sample);
        check(!wrong.inside || wrong.hit.byte_offset != b.hit.byte_offset,
              "an un-offset point does not resolve as if the field were at the origin");
    }

    // --- A point outside the field is not a tap on the field --------------------------------
    {
        android::CaretField target = field("hello");
        target.style = &style;
        target.metrics = &metrics;
        android::CaretSample sample;
        sample.x = 10.0f; // left of the box
        sample.y = 60.0f;
        check(!android::caret_from_point(target, sample).inside,
              "a point left of the box is outside");
        sample.x = 400.0f; // right of the box
        check(!android::caret_from_point(target, sample).inside,
              "a point right of the box is outside");
        sample.x = 150.0f;
        sample.y = 10.0f; // above the box
        check(!android::caret_from_point(target, sample).inside,
              "a point above the box is outside");
        sample.y = 200.0f; // below the box
        check(!android::caret_from_point(target, sample).inside,
              "a point below the box is outside");
    }

    // --- A point inside the box resolves to a caret ------------------------------------------
    {
        android::CaretField target = field("hello");
        target.style = &style;
        target.metrics = &metrics;
        android::CaretSample sample;
        // The very left of the content box: the caret goes before the first glyph.
        sample.x = 112.0f;
        sample.y = 62.0f;
        const android::CaretTranslation left = android::caret_from_point(target, sample);
        check(left.inside, "a point in the content box is inside");
        check(left.hit.byte_offset == 0, "the left content edge is offset 0");
    }

    // --- The density scale converts physical pixels to logical -------------------------------
    //
    // A Java MotionEvent reports physical pixels; the layout is in logical ones. Getting this
    // backwards puts every caret at the wrong character on a 2.4x phone.
    {
        android::CaretField target = field("hello");
        target.style = &style;
        target.metrics = &metrics;
        // The same logical point, expressed in physical pixels at scale 2.
        android::CaretSample logical;
        logical.x = 112.0f;
        logical.y = 62.0f;
        logical.dpi_scale = 1.0f;
        android::CaretSample physical;
        physical.x = 224.0f;
        physical.y = 124.0f;
        physical.dpi_scale = 2.0f;
        const android::CaretTranslation a = android::caret_from_point(target, logical);
        const android::CaretTranslation b = android::caret_from_point(target, physical);
        check(a.inside && b.inside, "both points are inside");
        check(a.hit.byte_offset == b.hit.byte_offset,
              "the density scale maps the same physical point to the same caret");
    }

    // --- A tap in the padding clamps to the nearest caret ------------------------------------
    //
    // A finger a few pixels off the text is still aiming at the field, so the padding must not
    // report "outside" and must not resolve to a negative offset.
    {
        android::CaretField target = field("hello");
        target.style = &style;
        target.metrics = &metrics;
        android::CaretSample sample;
        sample.x = 104.0f; // inside the box, inside the left padding
        sample.y = 62.0f;
        const android::CaretTranslation in_padding = android::caret_from_point(target, sample);
        check(in_padding.inside, "a point in the padding is still inside the field");
        check(in_padding.hit.byte_offset == 0, "the left padding clamps to offset 0");
    }

    // --- A point past the last glyph clamps to the end ---------------------------------------
    {
        android::CaretField target = field("hello");
        target.style = &style;
        target.metrics = &metrics;
        android::CaretSample sample;
        sample.x = 300.0f; // far right of the content box
        sample.y = 62.0f;
        const android::CaretTranslation past_end = android::caret_from_point(target, sample);
        check(past_end.inside, "a point past the text is inside the field");
        check(past_end.hit.byte_offset == 5, "a point past the last glyph clamps to the end");
    }

    // --- An empty field resolves to offset 0 -------------------------------------------------
    {
        android::CaretField target = field("");
        target.style = &style;
        target.metrics = &metrics;
        android::CaretSample sample;
        sample.x = 150.0f;
        sample.y = 62.0f;
        const android::CaretTranslation empty = android::caret_from_point(target, sample);
        check(empty.inside, "a point in an empty field is inside");
        check(empty.hit.byte_offset == 0, "an empty field has one caret position");
    }

    // --- A field with no style or metrics declines -------------------------------------------
    //
    // The shell loads the face once; a face that failed to load leaves the metrics null, and a tap
    // must then leave the caret alone rather than jumping it to the start.
    {
        android::CaretField target = field("hello");
        android::CaretSample sample;
        sample.x = 150.0f;
        sample.y = 62.0f;
        check(!android::caret_from_point(target, sample).inside, "a field with no style declines");
        target.style = &style;
        check(!android::caret_from_point(target, sample).inside,
              "a field with no metrics declines");
    }

    // --- A zero density scale does not divide ------------------------------------------------
    {
        android::CaretField target = field("hello");
        target.style = &style;
        target.metrics = &metrics;
        android::CaretSample sample;
        sample.x = 112.0f;
        sample.y = 62.0f;
        sample.dpi_scale = 0.0f;
        const android::CaretTranslation zero_scale = android::caret_from_point(target, sample);
        check(zero_scale.inside, "a zero density scale is treated as 1");
        check(zero_scale.hit.byte_offset == 0, "a zero density scale still resolves a caret");
    }

    if (failures != 0) {
        std::cerr << "android_caret: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_caret: ok\n";
    return 0;
}
