// SPDX-License-Identifier: MIT

// The simulated fold postures of the design-time studio (issue #408,
// docs/specs/designer-studio-v1.md section 6, docs/specs/safe-area-v1.md section 4): the ONE
// posture -> `FoldInfo` mapping, the `FoldChanged`-equivalent, and the rotation rule that swaps the
// two hinge axes.
//
// Everything here is a pure function of its arguments - no clock, no sensor, no OS - because a
// posture is data exactly like a viewport size. That is what makes the studio reproducible and what
// this test pins.

#include "tessera/hal/fold_posture.hpp"
#include "tessera/hal/safe_area.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

namespace hal = tessera::hal;

int failures = 0;

bool expect(bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "fold_posture: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.001f;
}

bool same_rect(const hal::WindowRect& got, float x, float y, float width, float height) {
    return near(got.x, x) && near(got.y, y) && near(got.width, width) && near(got.height, height);
}

// The four insets a full-occlusion hinge produces, as each posture's single-pane answer.
bool same(const hal::WindowInsets& got, float left, float top, float right, float bottom) {
    return near(got.left, left) && near(got.top, top) && near(got.right, right) &&
           near(got.bottom, bottom);
}

hal::SafeAreaConfig hinge_only() {
    hal::SafeAreaConfig config;
    config.sources = hal::kSourceHinge;
    return config;
}

void test_names_round_trip() {
    // Every posture the protocol names parses, and its name parses back: a studio control that
    // writes one name and reads another silently previews the wrong device.
    const hal::FoldPosture postures[] = {hal::FoldPosture::None, hal::FoldPosture::Flat,
                                         hal::FoldPosture::Book, hal::FoldPosture::Tabletop};
    for (const hal::FoldPosture posture : postures) {
        const std::string_view name = hal::fold_posture_name(posture);
        expect(!name.empty(), "a posture has a wire name");
        const std::optional<hal::FoldPosture> parsed = hal::parse_fold_posture(name);
        expect(parsed.has_value() && *parsed == posture,
               std::string("the name `") + std::string(name) + "` parses back to itself");
    }
    expect(hal::fold_posture_name(hal::FoldPosture::Book) == "book", "book is spelled `book`");
    expect(hal::fold_posture_name(hal::FoldPosture::Tabletop) == "tabletop",
           "tabletop is spelled `tabletop`");
    expect(hal::fold_posture_name(hal::FoldPosture::Flat) == "flat", "flat is spelled `flat`");

    // Anything else is refused rather than guessed at: an unknown posture must not look like
    // `flat`.
    expect(!hal::parse_fold_posture("").has_value(), "the empty string is not a posture");
    expect(!hal::parse_fold_posture("hinge").has_value(), "`hinge` is not a posture name");
    expect(!hal::parse_fold_posture("BOOK").has_value(), "posture names are case-sensitive");
}

void test_flat_has_no_hinge() {
    // `flat` is fully open: the state is reported, but there is no band and no occlusion, so the
    // hinge source contributes nothing to the safe area. That is what makes a round trip back to
    // `flat` restore the original frame.
    const hal::FoldInfo fold = hal::fold_info_for(hal::FoldPosture::Flat, 1000, 700);
    expect(fold.state == hal::FoldState::Flat, "flat reports `FoldState::Flat`");
    expect(fold.orientation == hal::FoldOrientation::None, "flat has no hinge axis");
    expect(fold.occlusion == hal::FoldOcclusion::None, "flat does not occlude");
    expect(!fold.separating, "flat is not separating");
    expect(same_rect(fold.hinge_bounds, 0.0f, 0.0f, 0.0f, 0.0f), "flat has a zero hinge rectangle");

    hal::WindowMetrics metrics;
    metrics.logical_width = 1000;
    metrics.logical_height = 700;
    metrics.fold = fold;
    expect(same(hal::resolve_safe_area(metrics, hinge_only()), 0, 0, 0, 0),
           "flat's hinge source resolves to no inset");
}

void test_book_splits_the_width() {
    // Book: half-opened, a VERTICAL hinge, full occlusion, separating. The band is centred on the
    // width and spans the full height, which is the rectangle a page draws as the crease.
    const hal::FoldInfo fold = hal::fold_info_for(hal::FoldPosture::Book, 1000, 700);
    expect(fold.state == hal::FoldState::HalfOpened, "book is half-opened");
    expect(fold.orientation == hal::FoldOrientation::Vertical, "book's hinge is vertical");
    expect(fold.occlusion == hal::FoldOcclusion::Full, "book's hinge fully occludes");
    expect(fold.separating, "book separates the two panes");
    expect(same_rect(fold.hinge_bounds, 488.0f, 0.0f, 24.0f, 700.0f),
           "book's band is centred on the width and spans the height");

    hal::WindowMetrics metrics;
    metrics.logical_width = 1000;
    metrics.logical_height = 700;
    metrics.fold = fold;
    // A single pane keeps the larger side. 1000 - (488 + 24) = 488 on the right, 488 on the left:
    // a tie keeps the first side (left), so the inset is on the RIGHT and the pane is 488 wide.
    const hal::WindowInsets insets = hal::resolve_safe_area(metrics, hinge_only());
    expect(same(insets, 0, 0, 512, 0),
           "book's full-occlusion hinge insets the right (tie keeps left)");
}

void test_tabletop_splits_the_height() {
    // Tabletop: half-opened, a HORIZONTAL hinge. The band is centred on the height and spans the
    // full width (content above, controls below).
    const hal::FoldInfo fold = hal::fold_info_for(hal::FoldPosture::Tabletop, 1000, 700);
    expect(fold.state == hal::FoldState::HalfOpened, "tabletop is half-opened");
    expect(fold.orientation == hal::FoldOrientation::Horizontal, "tabletop's hinge is horizontal");
    expect(fold.occlusion == hal::FoldOcclusion::Full, "tabletop's hinge fully occludes");
    expect(fold.separating, "tabletop separates the two panes");
    expect(same_rect(fold.hinge_bounds, 0.0f, 338.0f, 1000.0f, 24.0f),
           "tabletop's band is centred on the height and spans the width");

    hal::WindowMetrics metrics;
    metrics.logical_width = 1000;
    metrics.logical_height = 700;
    metrics.fold = fold;
    // 338 above, 700 - (338 + 24) = 338 below: another tie, so the bottom inset keeps the top pane.
    expect(same(hal::resolve_safe_area(metrics, hinge_only()), 0, 0, 0, 362),
           "tabletop's full-occlusion hinge insets the bottom (tie keeps top)");
}

void test_larger_side_wins() {
    // The full-occlusion rule is "a single pane keeps the LARGER side", and it lives in
    // `hal::resolve_safe_area` (issue #223). This test asserts the preview's postures reach it,
    // not that they re-implement it: shifting the same band proves the arithmetic decides, and the
    // band is a property of the window, not of the posture name.
    hal::WindowMetrics metrics;
    metrics.logical_width = 1000;
    metrics.logical_height = 700;
    metrics.fold.state = hal::FoldState::HalfOpened;
    metrics.fold.orientation = hal::FoldOrientation::Vertical;
    metrics.fold.occlusion = hal::FoldOcclusion::Full;
    metrics.fold.hinge_bounds = {200, 0, 40, 700};
    // 200 on the left, 760 on the right: the pane keeps the right, so the inset is on the left.
    expect(same(hal::resolve_safe_area(metrics, hinge_only()), 240, 0, 0, 0),
           "the larger right pane wins and the left is inset by the band");

    metrics.fold.hinge_bounds = {760, 0, 40, 700};
    // 760 on the left, 200 on the right: now the left wins, so the inset moves to the RIGHT and is
    // the distance from the band's leading edge to the window edge (1000 - 760 = 240).
    expect(same(hal::resolve_safe_area(metrics, hinge_only()), 0, 0, 240, 0),
           "the larger left pane wins and the right is inset by the band");
}

void test_zero_extent_and_tiny_windows() {
    // A zero extent has no axis to split, so the answer is the no-fold snapshot rather than a
    // degenerate rectangle.
    for (const hal::FoldPosture posture :
         {hal::FoldPosture::Flat, hal::FoldPosture::Book, hal::FoldPosture::Tabletop}) {
        const hal::FoldInfo fold = hal::fold_info_for(posture, 0, 0);
        expect(fold.hinge_bounds.width == 0.0f && fold.hinge_bounds.height == 0.0f,
               "a zero extent yields no hinge rectangle");
    }
    // A window narrower than the simulated band keeps a band of its own width, so neither pane can
    // go negative: 10 px window, 10 px band, 0 px of pane each.
    const hal::FoldInfo tiny = hal::fold_info_for(hal::FoldPosture::Book, 10, 10);
    expect(same_rect(tiny.hinge_bounds, 0.0f, 0.0f, 10.0f, 10.0f),
           "the band is clamped to the extent it splits");
}

void test_fold_changed() {
    // The `FoldChanged`-equivalent: a posture switch reports a change, and asking for the posture
    // already in force does not. This is what keeps an unchanged posture from costing a layout
    // pass.
    const hal::FoldInfo flat = hal::fold_info_for(hal::FoldPosture::Flat, 1000, 700);
    const hal::FoldInfo book = hal::fold_info_for(hal::FoldPosture::Book, 1000, 700);
    expect(!hal::fold_changed(book, book), "the same fold is not a change");
    expect(hal::fold_changed(flat, book), "flat -> book is a change");
    expect(hal::fold_changed(book, flat), "book -> flat is a change");
    expect(hal::fold_changed(book, hal::fold_info_for(hal::FoldPosture::Tabletop, 1000, 700)),
           "book -> tabletop changes the axis");

    // A pure band move is a change too: the rectangle is what `resolve_safe_area` reads.
    hal::FoldInfo moved = book;
    moved.hinge_bounds.x = 100.0f;
    expect(hal::fold_changed(book, moved), "a moved hinge band is a change");

    // ... and so is a state / occlusion / separating change on its own.
    hal::FoldInfo opened = book;
    opened.state = hal::FoldState::Flat;
    expect(hal::fold_changed(book, opened), "a state change alone is a change");
}

void test_rotate_swaps_the_axes() {
    // A rotation of a foldable swaps the roles of the two hinge axes (safe-area-v1.md section 4,
    // responsive.md section 5): the book fold seen in the rotated frame is horizontal, and it is
    // re-centred in the rotated extent. This is the issue's "test for the rotated case".
    const hal::FoldInfo book = hal::fold_info_for(hal::FoldPosture::Book, 1000, 700);
    const hal::FoldInfo rotated = hal::rotate_fold(book, 700, 1000);
    expect(rotated.orientation == hal::FoldOrientation::Horizontal,
           "a rotated book fold becomes horizontal");
    expect(rotated.state == hal::FoldState::HalfOpened, "rotation keeps the half-opened state");
    expect(rotated.occlusion == hal::FoldOcclusion::Full, "rotation keeps the occlusion");
    expect(rotated.separating, "rotation keeps the separating flag");
    // The band keeps its thickness (24) and is re-centred on the new height (1000).
    expect(same_rect(rotated.hinge_bounds, 0.0f, 488.0f, 700.0f, 24.0f),
           "the rotated band keeps its thickness and is re-centred on the new axis");

    const hal::FoldInfo tabletop = hal::fold_info_for(hal::FoldPosture::Tabletop, 1000, 700);
    const hal::FoldInfo swapped = hal::rotate_fold(tabletop, 700, 1000);
    expect(swapped.orientation == hal::FoldOrientation::Vertical,
           "a rotated tabletop fold becomes vertical");
    expect(same_rect(swapped.hinge_bounds, 338.0f, 0.0f, 24.0f, 1000.0f),
           "the rotated tabletop band is re-centred on the new width");

    // Rotating twice returns the geometry (the thickness is preserved exactly).
    const hal::FoldInfo twice = hal::rotate_fold(rotated, 1000, 700);
    expect(twice.orientation == hal::FoldOrientation::Vertical, "two rotations restore the axis");
    expect(same_rect(twice.hinge_bounds, book.hinge_bounds.x, book.hinge_bounds.y,
                     book.hinge_bounds.width, book.hinge_bounds.height),
           "two rotations restore the band");

    // A flat fold has no axis to swap, so it is returned untouched.
    const hal::FoldInfo flat = hal::fold_info_for(hal::FoldPosture::Flat, 1000, 700);
    expect(!hal::fold_changed(flat, hal::rotate_fold(flat, 700, 1000)),
           "rotating a flat fold changes nothing");
}

} // namespace

int main() {
    test_names_round_trip();
    test_flat_has_no_hinge();
    test_book_splits_the_width();
    test_tabletop_splits_the_height();
    test_larger_side_wins();
    test_zero_extent_and_tiny_windows();
    test_fold_changed();
    test_rotate_swaps_the_axes();
    if (failures == 0) {
        std::cout << "fold_posture: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
