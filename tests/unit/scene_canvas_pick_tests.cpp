// SPDX-License-Identifier: MIT
//
// The picker overlay's engine half (issue #171's picker slice): what is under a point, named and
// boxed, without changing the scene.
//
// The acceptance sentence has three clauses, and each is asserted here:
//
//  1. **The right node.** A pick at a known position returns the node the pointer is over - the
//  same
//     node the router's hit test would reach, because it *is* the router's hit test. The test reads
//     the node's own `automation_id()` and `world_bounds()` from the tree and compares, so a
//     passing assertion is evidence the session reported the tree rather than a constant.
//  2. **Nothing outside any node.** A point outside the viewport is `found:false` with a zero box,
//     which is the page's signal to hide the overlay instead of drawing a stale outline.
//  3. **A query, not an event.** A pick moves no pseudo-class, marks no dirty domain and owes no
//     frame: arming the picker and moving over a button must not repaint the canvas or run a
//     handler. This is the clause that separates the picker from `pointer()`, and it is why the
//     export is not a pointer kind.
//
// The report serializer is asserted too, because the page parses it: the box is four JSON numbers
// in logical pixels, and a miss is a well-formed document rather than an error.

#include "scene_canvas_test_support.hpp"

#include <string>

namespace {

using namespace canvas_test;

// The box a node reports, as the test's own reading of the tree. Compared field by field rather
// than as a whole so a failure names the edge that moved.
void expect_box(const rhi::Rect& actual, const rhi::Rect& expected, std::string_view what) {
    expect(actual.x == expected.x, std::string(what) + ": box x is the node's own x");
    expect(actual.y == expected.y, std::string(what) + ": box y is the node's own y");
    expect(actual.width == expected.width,
           std::string(what) + ": box width is the node's own width");
    expect(actual.height == expected.height,
           std::string(what) + ": box height is the node's own height");
}

void test_pick_returns_the_node_under_the_point() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");
    const ui::Node* button = find_kind(*session.root(), "Button");
    expect(button != nullptr, "the source has a button");
    if (button == nullptr) {
        return;
    }
    const Point centre = centre_of(*button);

    const cli::CanvasPickResult picked = session.pick(centre.x, centre.y);
    expect(picked.found, "a pick at the button's centre finds a node");
    expect(picked.automation_id == button->automation_id(),
           "the picked node is the button the point is over");
    expect(picked.element_kind == "Button", "the pick names the node's element kind, got `" +
                                                std::string(picked.element_kind) + "`");
    expect_box(picked.box, button->world_bounds(), "the picked button");

    // A point inside the button but off its centre picks the same node: the box is a rectangle, not
    // a point, and an overlay that only worked at the centre would outline nothing under a real
    // pointer.
    const rhi::Rect bounds = button->world_bounds();
    const cli::CanvasPickResult corner = session.pick(bounds.x + 1.0f, bounds.y + 1.0f);
    expect(corner.found, "a pick near the button's top-left corner finds a node");
    expect(corner.automation_id == button->automation_id(),
           "the corner of the box is still the button");
}

void test_pick_returns_nothing_outside_any_node() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");

    // `kOutside` is the same point the pointer tests use for a leave: outside the 320x240 layout
    // viewport, where no node covers anything.
    const cli::CanvasPickResult outside = session.pick(kOutside.x, kOutside.y);
    expect(!outside.found, "a pick outside every node finds nothing");
    expect(outside.automation_id == 0, "a miss reports no id");
    expect(outside.element_kind.empty(), "a miss reports no element kind");
    expect(outside.box.width == 0.0f && outside.box.height == 0.0f, "a miss reports a zero box");

    // A session with no tree answers the same way rather than crashing: the page arms the picker
    // before the first Run, and a null tree is the ordinary state at that moment.
    cli::CanvasSceneSession empty;
    const cli::CanvasPickResult none = empty.pick(10.0f, 10.0f);
    expect(!none.found, "an unstarted session picks nothing");
}

// The clause that makes the picker a query: it must be invisible to the scene. If this ever fails,
// arming the picker would repaint the canvas on every pointer move - the animation loop the canvas
// session exists to avoid - and would leave a `:hover` the user never asked for.
void test_pick_changes_nothing() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");
    const ui::Node* button = find_kind(*session.root(), "Button");
    if (button == nullptr) {
        expect(false, "the counter has a button");
        return;
    }
    const Point centre = centre_of(*button);
    const std::string resting = background_of(session, *button);

    CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the harness has a device and a target");
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");
    expect(canvas.frames == 1, "one frame so far: the Run");

    // Ten picks over the button: no hover, no dirty domain, no frame.
    for (int attempt = 0; attempt < 10; ++attempt) {
        const cli::CanvasPickResult picked = session.pick(centre.x, centre.y);
        expect(picked.found, "a pick over the button finds it");
    }
    expect(session.router() != nullptr && session.router()->hovered() == nullptr,
           "a pick sets no hover: the router's hover target is still null");
    expect(background_of(session, *button) == resting,
           "a pick leaves the button at its resting colour, so no :hover rule matched");
    expect(!session.needs_frame(), "a pick owes no frame");
    expect(!canvas.draw(session), "a pick paints nothing");
    expect(canvas.frames == 1, "ten picks cost zero frames");
    expect(counter_value(session) == 0, "a pick runs no handler");
}

void test_pick_report_json() {
    cli::CanvasPickResult result;
    result.found = true;
    result.automation_id = 101;
    result.element_kind = "Button";
    result.box = rhi::Rect{12.0f, 40.0f, 120.0f, 32.0f};
    const std::string report = cli::canvas_pick_report_json(result);
    expect(report.find("\"pickVersion\":1") != std::string::npos,
           "the pick report carries its version");
    expect(report.find("\"found\":true") != std::string::npos, "the report says a node was found");
    // Ids are decimal strings, like every other id in this module: a 64-bit uiId does not survive
    // JSON.parse as a JS number.
    expect(report.find("\"automationId\":\"101\"") != std::string::npos,
           "the id is a quoted decimal string");
    expect(report.find("\"elementKind\":\"Button\"") != std::string::npos,
           "the report names the element kind");
    // The box is four numbers in logical pixels, so the page can position the outline without a
    // second call. Whole-valued edges print without a decimal point.
    expect(report.find("\"box\":{\"x\":12,\"y\":40,\"width\":120,\"height\":32}") !=
               std::string::npos,
           "the box is four numbers in logical pixels, got " + report);
    expect(report.back() == '\n', "the report is one newline-terminated line");

    // A miss is a well-formed document, not an error: the page reads `found` and hides the overlay.
    const std::string miss = cli::canvas_pick_report_json(cli::CanvasPickResult{});
    expect(miss.find("\"found\":false") != std::string::npos, "a miss reports found:false");
    expect(miss.find("\"automationId\":\"0\"") != std::string::npos, "a miss reports id 0");
    expect(miss.find("\"elementKind\":\"\"") != std::string::npos,
           "a miss reports no element kind");
    expect(miss.find("\"box\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0}") != std::string::npos,
           "a miss reports a zero box, got " + miss);

    // A fractional edge keeps its fraction: a box at 12.5 logical pixels is a real layout result,
    // and rounding it would draw the outline half a pixel off.
    cli::CanvasPickResult fractional;
    fractional.found = true;
    fractional.box = rhi::Rect{12.5f, 0.0f, 10.25f, 1.0f};
    const std::string precise = cli::canvas_pick_report_json(fractional);
    expect(precise.find("\"x\":12.5") != std::string::npos,
           "a fractional edge keeps its fraction, got " + precise);
    expect(precise.find("\"width\":10.25") != std::string::npos,
           "a fractional width keeps its fraction");
}

} // namespace

int main() {
    test_pick_returns_the_node_under_the_point();
    test_pick_returns_nothing_outside_any_node();
    test_pick_changes_nothing();
    test_pick_report_json();
    if (failures() != 0) {
        std::cerr << "scene_canvas_pick: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_pick: ok\n";
    return 0;
}
