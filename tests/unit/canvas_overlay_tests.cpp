// SPDX-License-Identifier: MIT
//
// The debug overlay document (issue #433): the hinge band and the safe areas the previewed tree was
// laid out against.
//
// **What this pins, and why it is the engine's answer rather than the page's.** A posture is data
// (`fold_posture.hpp`), and the page knows which one it asked for, so drawing a band from the
// page's own arithmetic would be easy and wrong: it would be a second mapping from `book` to a
// rectangle, free to disagree with the layout it exists to explain. The overlay is therefore
// serialized from `WindowMetrics` - the snapshot the tree was laid out with, through the same
// `hal::fold_info_for()` the layout consumed - and this test is that round trip: a posture in, the
// band the layout got out.
//
// The document is also a *query*: nothing is dispatched and no frame is owed, which is a property
// of the export (`wasm_canvas_query.cpp`), not of the serializer. What is testable here is the
// exact bytes, so a page change that reads a field the serializer stopped writing is caught.

#include "canvas_overlay_report.hpp"
// The posture -> `FoldInfo` mapping the layout itself calls (`simulated_metrics`). Included so this
// test builds its metrics the way the session does rather than restating a band calculation.
#include "tessera/hal/fold_posture.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace hal = tessera::hal;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "canvas_overlay: " << message << "\n";
        ++failures;
    }
}

// The metrics a preview at `posture` lays out against, built the same way the session builds them
// (`simulated_metrics` in `scene_layout.cpp`): the posture resolves through `fold_info_for`, so
// this half of the test is the mapping the layout itself uses, not a second one written here.
hal::WindowMetrics metrics_at(hal::FoldPosture posture, std::uint32_t width, std::uint32_t height) {
    hal::WindowMetrics metrics;
    metrics.logical_width = width;
    metrics.logical_height = height;
    metrics.fold = hal::fold_info_for(posture, width, height);
    return metrics;
}

bool contains(std::string_view document, std::string_view fragment) {
    return document.find(fragment) != std::string_view::npos;
}

// A `book` fold's band is vertical, centred on the width, and spans the full height. The overlay
// has to report exactly the rectangle the layout used, because that is the whole reason it is not
// drawn from the page's own arithmetic.
void a_book_hinge_is_the_rectangle_the_layout_used() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::Book, 800, 600));
    expect(contains(document, "\"foldState\":\"half-opened\""), "book reports half-opened");
    expect(contains(document, "\"foldOrientation\":\"vertical\""), "book's hinge is vertical");
    expect(contains(document, "\"foldOcclusion\":\"full\""), "the simulated hinge occludes");
    expect(contains(document, "\"separating\":true"), "the simulated hinge is separating");
    // 800 wide, a 24 px band centred: x = (800 - 24) / 2 = 388, full height.
    expect(contains(document, "\"hinge\":{\"x\":388,\"y\":0,\"width\":24,\"height\":600}"),
           "the band is centred on the width and spans the height");

    // And it is the same rectangle the engine's own safe-area arithmetic reads, so the picture and
    // the layout cannot drift apart.
    const hal::WindowMetrics metrics = metrics_at(hal::FoldPosture::Book, 800, 600);
    expect(metrics.fold.hinge_bounds.x == 388.0f && metrics.fold.hinge_bounds.width == 24.0f,
           "the band is the one the layout resolved");
}

void a_tabletop_hinge_is_horizontal() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::Tabletop, 800, 600));
    expect(contains(document, "\"foldOrientation\":\"horizontal\""),
           "tabletop's hinge is horizontal");
    // 600 tall, a 24 px band centred: y = (600 - 24) / 2 = 288, full width.
    expect(contains(document, "\"hinge\":{\"x\":0,\"y\":288,\"width\":800,\"height\":24}"),
           "the band is centred on the height and spans the width");
}

// `flat` is a foldable that is open: a state, no band. The overlay draws nothing, and reports the
// state it is drawing nothing for, so the pane can say "flat" rather than "unknown".
void flat_has_a_state_and_no_band() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::Flat, 800, 600));
    expect(contains(document, "\"foldState\":\"flat\""), "flat reports its state");
    expect(contains(document, "\"foldOrientation\":\"none\""), "flat has no axis to orient");
    expect(contains(document, "\"hinge\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0}"),
           "a flat fold has a zero-area band, which is the page's 'draw nothing'");
}

void no_fold_reports_none() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::None, 800, 600));
    expect(contains(document, "\"foldState\":\"none\""), "no fold reports none");
    expect(contains(document, "\"hinge\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0}"),
           "no fold has a zero-area band");
}

// The angle a simulated posture reports is "nothing measured one", and the page must be able to
// tell that from a real zero (which is a device folded flat shut). -1 is the HAL's own sentinel.
void a_simulated_posture_reports_no_hinge_angle() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::Book, 800, 600));
    expect(contains(document, "\"hingeAngle\":-1"),
           "a simulated posture reports the HAL's unknown-angle sentinel, not 0");
}

// The insets the skin handed the viewer are the ones the overlay shades, and they are reported even
// when they are all zero: a page reading a missing field would have to guess between "no insets"
// and "an older module".
void the_insets_are_reported_as_the_layout_resolved_them() {
    hal::WindowMetrics metrics = metrics_at(hal::FoldPosture::None, 390, 844);
    metrics.insets = {.left = 0.0f, .top = 47.0f, .right = 0.0f, .bottom = 34.0f};
    metrics.ime_insets = {.left = 0.0f, .top = 0.0f, .right = 0.0f, .bottom = 291.0f};
    const std::string document = cli::canvas_overlay_report_json(metrics);
    expect(contains(document, "\"insets\":{\"left\":0,\"top\":47,\"right\":0,\"bottom\":34}"),
           "the system-bar insets are reported");
    expect(contains(document, "\"ime\":{\"left\":0,\"top\":0,\"right\":0,\"bottom\":291}"),
           "the keyboard inset is reported separately, so the overlay can shade it differently");
    expect(contains(document, "\"gestures\":{\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"),
           "a zero inset set is reported as zeros, not omitted");
}

// A cutout is a rectangle, and only the reported ones are written: an entry past `count` is a zero
// the HAL left there, and drawing it would show a notch on a device that has none.
void only_the_reported_cutouts_are_written() {
    hal::WindowMetrics metrics = metrics_at(hal::FoldPosture::None, 390, 844);
    metrics.cutouts.count = 1;
    metrics.cutouts.bounds[0] = {.x = 130.0f, .y = 0.0f, .width = 130.0f, .height = 30.0f};
    const std::string document = cli::canvas_overlay_report_json(metrics);
    expect(contains(document, "\"cutouts\":[{\"x\":130,\"y\":0,\"width\":130,\"height\":30}]"),
           "one reported cutout is one rectangle");
    expect(!contains(document, "\"cutouts\":[{\"x\":130,\"y\":0,\"width\":130,\"height\":30},{"),
           "the zero bounds past `count` are not written");
}

void no_cutouts_is_an_empty_list() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::None, 390, 844));
    expect(contains(document, "\"cutouts\":[]"), "a device with no cutout reports an empty list");
}

// The document is one line: the page parses it with `JSON.parse` off a C string, and a stray
// newline inside would be harmless there but would break a line-oriented reader (the CLI and the
// probes).
void the_document_is_one_line() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::Book, 800, 600));
    expect(!document.empty() && document.back() == '\n', "the document ends with one newline");
    expect(document.find('\n') == document.size() - 1, "there is no newline before the last one");
    expect(document.rfind("{\"overlayVersion\":1", 0) == 0,
           "the document starts with its version, like every other report in this module");
}

// A posture in a degenerate extent has no axis to split, so `fold_info_for` answers the no-fold
// snapshot. The overlay follows rather than inventing a band the layout never used.
void a_zero_extent_has_no_band() {
    const std::string document =
        cli::canvas_overlay_report_json(metrics_at(hal::FoldPosture::Book, 0, 0));
    expect(contains(document, "\"hinge\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0}"),
           "no extent, no band");
}

// A half-pixel edge prints as the shortest round-trip form, never with a locale's decimal comma:
// the page parses this document with `JSON.parse`, which would read `183,5` as two values.
void a_fractional_edge_prints_as_shortest_json() {
    hal::WindowMetrics metrics = metrics_at(hal::FoldPosture::None, 400, 300);
    metrics.insets.top = 183.5f;
    const std::string document = cli::canvas_overlay_report_json(metrics);
    expect(contains(document, "\"top\":183.5"), "a fractional inset prints with a point");
    expect(!contains(document, "183,5"), "and never with a decimal comma");
}

int run() {
    a_book_hinge_is_the_rectangle_the_layout_used();
    a_tabletop_hinge_is_horizontal();
    flat_has_a_state_and_no_band();
    no_fold_reports_none();
    a_simulated_posture_reports_no_hinge_angle();
    the_insets_are_reported_as_the_layout_resolved_them();
    only_the_reported_cutouts_are_written();
    no_cutouts_is_an_empty_list();
    the_document_is_one_line();
    a_zero_extent_has_no_band();
    a_fractional_edge_prints_as_shortest_json();
    if (failures == 0) {
        std::cout << "canvas_overlay: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main() {
    return run();
}
