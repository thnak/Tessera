// SPDX-License-Identifier: MIT
//
// The caret box paint draws, exposed so a platform IME can answer `requestCursorUpdates` with the
// SAME geometry (issue #189, docs/guides/android-lane.md section 3.18).
//
// The rule that bites: an IME that gets a different box places its suggestion strip, its
// autocorrect highlight and its magnifier in the wrong place. Both callers therefore go through one
// function, and this test pins what that function promises: the box is on the caret's line, its x
// is the advance of the prefix, and its width follows the device scale the way the painted caret
// does.
//
// The rules are pure over a laid-out `TextLayout`, so they run on every lane with no GPU and no
// device.

#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <iostream>
#include <string_view>

namespace {

namespace paint = tessera::ui::paint;
namespace text = tessera::ui::text;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    const paint::StubPaintTextMetrics source;
    const text::TextStyle style{};

    // One line, "hello", laid out with the stub metrics so the advances are deterministic.
    text::TextConstraints constraints;
    constraints.wrap = false;
    const text::TextLayout layout = source.layout("hello", style, constraints);
    check(!layout.lines.empty(), "the layout has a line");

    // --- The caret at offset 0 sits at the line's left edge --------------------------------
    {
        const paint::CaretBox box = paint::caret_box("hello", layout, 0, style, source, 1.0f);
        check(box.x == 0.0f, "the caret at offset 0 is at x=0");
        check(box.y == 0.0f, "the first line starts at y=0");
        check(box.height > 0.0f, "the box has the line's height");
    }

    // --- The caret's x is the advance of the prefix ----------------------------------------
    //
    // This is the whole contract: the box must land where the glyphs are, not at a guessed offset.
    {
        const paint::CaretBox at_zero = paint::caret_box("hello", layout, 0, style, source, 1.0f);
        const paint::CaretBox at_two = paint::caret_box("hello", layout, 2, style, source, 1.0f);
        const paint::CaretBox at_end = paint::caret_box("hello", layout, 5, style, source, 1.0f);
        check(at_two.x > at_zero.x, "a later caret is further right");
        check(at_end.x > at_two.x, "the end caret is furthest right");
        // The end caret's x is the line's width, which is what the layout measured.
        check(at_end.x == layout.lines.front().width,
              "the end caret sits at the line's measured width");
    }

    // --- The width follows the device scale, like the painted caret ------------------------
    //
    // Paint draws 1 px at scale 1 and 2 px at >= 2. An IME that got a different width would place
    // its magnifier a pixel off, which is visible.
    {
        const paint::CaretBox at_one = paint::caret_box("hello", layout, 2, style, source, 1.0f);
        const paint::CaretBox at_two = paint::caret_box("hello", layout, 2, style, source, 2.0f);
        check(at_one.width == 1.0f, "the caret is 1 px at device scale 1");
        check(at_two.width == 2.0f, "the caret is 2 px at device scale 2");
        check(at_one.x == at_two.x, "the scale changes the width, not the position");
    }

    // --- An offset past the end clamps to the end ------------------------------------------
    {
        const paint::CaretBox past = paint::caret_box("hello", layout, 999, style, source, 1.0f);
        const paint::CaretBox at_end = paint::caret_box("hello", layout, 5, style, source, 1.0f);
        check(past.x == at_end.x, "an offset past the end clamps to the end");
    }

    // --- An empty layout has no box --------------------------------------------------------
    {
        const text::TextLayout empty;
        const paint::CaretBox box = paint::caret_box("", empty, 0, style, source, 1.0f);
        check(box.x == 0.0f && box.y == 0.0f && box.height == 0.0f,
              "an empty layout reports an empty box");
    }

    // --- A line origin offsets the box -----------------------------------------------------
    //
    // A centred or end-aligned line starts right of the content box; paint passes that origin so
    // the box lands on the glyphs rather than at the content box's left edge.
    {
        const paint::CaretBox shifted =
            paint::caret_box("hello", layout, 2, style, source, 1.0f, 40.0f, 10.0f);
        const paint::CaretBox plain = paint::caret_box("hello", layout, 2, style, source, 1.0f);
        check(shifted.x == plain.x + 40.0f, "the line origin shifts the box horizontally");
        check(shifted.y == plain.y + 10.0f, "the line origin shifts the box vertically");
    }

    if (failures != 0) {
        std::cerr << "paint_caret_box: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "paint_caret_box: ok\n";
    return 0;
}
