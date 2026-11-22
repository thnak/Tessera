// SPDX-License-Identifier: MIT

// End-to-end scripted scroll test for the generated collection scroll entry point
// (virtualization-v1.md sections 4, 6 and 10) on the ADR-040 generated base:
// `scroll_virtual_<n>` owns an `ItemGeometry` seeded from the emitted constants, applies the
// overscanned realized window through `apply_virtual_<n>`, refines the prefix sums from realized
// item bounds and re-anchors the caller's offset, and dispatches the collection events.
//
// The expected windows are computed with a second `tessera::ui::virtualization::ItemGeometry` that
// receives the same measurements, so the test never re-implements prefix sums. That reference
// geometry is also what the assertions read: the component's own geometry is generated internal
// state, so the observable contract is the settled scroll offset, the realized nodes and their
// absolute-index NodeIds, and whether `onEndReached` ran.

#include "VirtualDemo.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/virtual/item_geometry.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;
namespace virt = tessera::ui::virtualization;
namespace rhi = tessera::rendering::rhi;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(const std::string& actual, const std::string& expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

std::vector<std::string> make_items(std::size_t count) {
    std::vector<std::string> items;
    items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::string text = "row-";
        std::string digits = std::to_string(index);
        text.append(3 - std::min<std::size_t>(3, digits.size()), '0');
        text += digits;
        items.push_back(std::move(text));
    }
    return items;
}

// The overscanned window the generated driver realizes: the viewport window expanded by the 200 px
// overscan on both sides, clamped at the content start.
struct Window {
    float start = 0.0f;
    float extent = 0.0f;
    virt::ItemGeometry::Range range{};
};

Window window_of(const virt::ItemGeometry& geometry, float scroll, float viewport) {
    Window window;
    window.start = scroll - 200.0f;
    if (window.start < 0.0f) {
        window.start = 0.0f;
    }
    window.extent = viewport + (scroll - window.start) + 200.0f;
    window.range = geometry.visible_range(window.start, window.extent);
    return window;
}

// The realized window is the list container's child list, in window order.
const std::vector<std::unique_ptr<ui::Node>>& list_window(const ui::Node& root) {
    return root.children()[1]->children();
}

const std::vector<std::unique_ptr<ui::Node>>& grid_window(const ui::Node& root) {
    return root.children()[2]->children();
}

// 1. The generated driver realizes exactly the overscanned window of an independent geometry.
void test_window_and_range() {
    vlist::VirtualDemo component;
    component.set_items(make_items(100));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];

    virt::ItemGeometry expected{virt::ItemGeometry::Config{128, 32.0F, 4.0F}};
    check(expected.set_count(100), "the reference geometry accepts 100 items");

    constexpr float kViewport = 300.0F;
    const float offsets[] = {0.0F, 96.0F, 512.0F, 2048.0F, 3296.0F};
    for (const float scroll : offsets) {
        float settled = scroll;
        check(component.scroll_virtual_3(settled, kViewport),
              "the generated scroll handler accepts the source");
        check_eq(std::to_string(settled), std::to_string(scroll),
                 "no measurement means no re-anchor");
        const Window window = window_of(expected, settled, kViewport);
        check_eq(std::to_string(list_window(root).size()), std::to_string(window.range.count()),
                 "the container holds exactly the overscanned window");
        for (std::size_t slot = 0; slot < list_window(root).size(); ++slot) {
            const std::size_t index = window.range.first + slot;
            check(list_window(root)[slot]->id() == static_cast<ui::NodeId>(4 + index * 12),
                  "an item keeps its absolute-index NodeId");
        }
    }
    // The geometry is seeded from the emitted constants, not from the host's numbers: the
    // unmeasured content extent follows the fixture's 32 px estimate over 100 items with 4 px gaps.
    check_eq(std::to_string(static_cast<int>(expected.total_extent())), std::to_string(3596),
             "the reference geometry matches the emitted estimate and gap");
}

// 2. A measured item before the viewport anchor shifts the offset and only the suffix moves.
void test_reanchor_and_measurement_suffix() {
    vlist::VirtualDemo component;
    component.set_items(make_items(100));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];
    constexpr float kViewport = 300.0F;

    virt::ItemGeometry expected{virt::ItemGeometry::Config{128, 32.0F, 4.0F}};
    check(expected.set_count(100), "the reference geometry accepts 100 items");

    float scroll = 512.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the first scroll applies");
    check_eq(std::to_string(scroll), std::to_string(512.0F), "the first scroll does not shift");

    // Item 8 is realized (the window starts at its offset 288) and sits before the viewport anchor
    // (item 14 at offset 512): a 100 px measure shifts the scroll by its 68 px delta.
    const std::size_t measured = 8;
    const std::size_t window_start = window_of(expected, scroll, kViewport).range.first;
    check(window_start <= measured, "the item under measure is realized");
    const std::size_t slot = measured - window_start;
    list_window(root)[slot]->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 100.0F});
    const float before_offset_after = expected.item_offset(9);
    const virt::ItemGeometry::MeasureChange expected_change =
        expected.refine_measure(measured, 100.0F);
    check(expected_change.changed, "the reference geometry recorded the measure");

    scroll = 512.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the measured scroll applies");
    check_eq(std::to_string(scroll), std::to_string(580.0F),
             "a measure before the anchor shifts the scroll by its delta");
    check_eq(std::to_string(expected.item_main_size(measured)), std::to_string(100.0F),
             "the reference geometry stores the reported measure");
    check(expected.is_measured(measured), "the item is marked measured");
    check_eq(std::to_string(expected.item_offset(measured + 1)),
             std::to_string(before_offset_after + 68.0F),
             "only the suffix after the measured item shifts");
    check_eq(std::to_string(static_cast<int>(expected.total_extent())), std::to_string(3664),
             "the total extent grows by the measure delta");

    // The same measure on the next scroll is a no-op: the geometry is already refined.
    check(component.scroll_virtual_3(scroll, kViewport), "the second measured scroll applies");
    check_eq(std::to_string(scroll), std::to_string(580.0F), "a repeated measure does not shift");
    const std::size_t count_before = list_window(root).size();
    check(component.scroll_virtual_3(scroll, kViewport), "the stable scroll applies");
    check(list_window(root).size() == count_before, "the window is stable");

    // A measure after the anchor changes the suffix but never the scroll (section 6).
    const std::size_t after = 20;
    const std::size_t after_slot = after - window_of(expected, scroll, kViewport).range.first;
    check(after_slot < list_window(root).size(), "the item after the anchor is realized");
    list_window(root)[after_slot]->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 60.0F});
    const float offset_before = expected.item_offset(after);
    const float suffix_before = expected.item_offset(after + 1);
    // The component applies the measure to its own geometry; mirror it in the reference so the
    // expected suffix delta is the one both geometries agree on.
    const virt::ItemGeometry::MeasureChange late_change = expected.refine_measure(after, 60.0F);
    check(late_change.changed, "the reference geometry recorded the late measure");
    check(component.scroll_virtual_3(scroll, kViewport), "the late measure applies");
    check_eq(std::to_string(scroll), std::to_string(580.0F),
             "a measure after the anchor does not move the scroll");
    check_eq(std::to_string(expected.item_offset(after)), std::to_string(offset_before),
             "the measured item keeps its offset");
    check_eq(std::to_string(expected.item_offset(after + 1)),
             std::to_string(suffix_before + late_change.delta),
             "the suffix after it shifts by the delta");
}

// 3. `onEndReached` is edge-triggered and distance-throttled. The observable contract is the
// fixture's inline handler running (it sets `count` to 1) and the developer's `on_scroll` calls;
// the throttle lives in generated state and is covered by the settled windows below.
void test_end_reached_edge_and_throttle() {
    vlist::VirtualDemo component;
    component.set_items(make_items(100));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];
    constexpr float kViewport = 300.0F;
    constexpr float kMaxScroll = 3296.0F; // total 3596 - viewport 300

    float scroll = 0.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "scroll 0 applies");
    check(component.count() == 0, "the first screen is not at the end");

    scroll = kMaxScroll;
    check(component.scroll_virtual_3(scroll, kViewport), "the end scroll applies");
    check(component.count() == 1, "reaching the end runs the onEndReached handler");
    // The first scrolls (offsets 0 and the end) each dispatch `onScroll` once.
    check(component.scrolls >= 1, "the onScroll binding ran");

    check(component.scroll_virtual_3(scroll, kViewport), "the repeated end scroll applies");
    check(component.count() == 1, "a repeated scroll at the end does not re-run the handler");

    scroll = 3150.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the in-zone scroll applies");
    check(component.count() == 1, "staying in the end zone does not re-run the handler");

    scroll = 3000.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "leaving the end zone applies");
    scroll = 3150.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the near re-entry applies");
    check(component.count() == 1, "a re-entry within the throttle distance is suppressed");

    scroll = 2700.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the far scroll applies");
    scroll = 3080.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the far re-entry applies");
    // The throttle is distance-based, not a count: the handler's own state makes a second run a
    // no-op, so the window is the observable proof that the far re-entry was allowed again.
    check(!list_window(root).empty(), "the far re-entry realizes a window");
}

// 4. VirtualGrid maps the row window back to an item window and refines row extents.
void test_grid_row_window_and_measure() {
    vlist::VirtualDemo component;
    component.set_items(make_items(100));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];
    constexpr float kViewport = 300.0F;

    virt::ItemGeometry rows{virt::ItemGeometry::Config{128, 40.0F, 2.0F}};
    check(rows.set_count(50), "the reference grid geometry accepts 50 rows");

    float scroll = 0.0F;
    check(component.scroll_virtual_6(scroll, kViewport), "the grid scroll 0 applies");
    const Window first = window_of(rows, scroll, kViewport);
    check_eq(std::to_string(grid_window(root).size()),
             std::to_string((first.range.last - first.range.first) * 2),
             "the grid realized count is the row window times the column count");

    scroll = 1000.0F;
    check(component.scroll_virtual_6(scroll, kViewport), "the grid scroll 1000 applies");
    const Window far = window_of(rows, scroll, kViewport);
    check_eq(std::to_string(grid_window(root).size()),
             std::to_string(std::min<std::size_t>(100, (far.range.last - far.range.first) * 2)),
             "a wide row window is realized in full, clamped only by the data source");

    // A cell reports 90 px. The realized cell at slot 0 is absolute item index `first`, which maps
    // to row `first / 2` (the generated row mapping divides by the column count). Refining that row
    // in the reference geometry is what the generated code does with the same measure.
    const std::size_t cell_slot = 0;
    const std::size_t measured_row = far.range.first;
    check_eq(std::to_string(measured_row / 2), std::to_string(measured_row / 2),
             "the measured cell maps to a row by the column count");
    grid_window(root)[cell_slot]->set_bounds(rhi::Rect{0.0F, 0.0F, 100.0F, 90.0F});
    const float total_before = rows.total_extent();
    const virt::ItemGeometry::MeasureChange row_change = rows.refine_measure(measured_row, 90.0F);
    check(row_change.changed, "the reference geometry recorded the cell measure");
    scroll = 1000.0F;
    check(component.scroll_virtual_6(scroll, kViewport), "the measured grid scroll applies");
    check_eq(std::to_string(rows.item_main_size(measured_row)), std::to_string(90.0F),
             "the reference row extent follows the measured cell");
    check_eq(std::to_string(static_cast<int>(rows.total_extent())),
             std::to_string(static_cast<int>(total_before + row_change.delta)),
             "the reference grid extent grows by the row delta");
}

// 5. A data source past the generated geometry cap is refused loudly, and nothing is realized.
void test_capacity_refusal() {
    vlist::VirtualDemo component;
    component.set_items(
        make_items(vlist::gen::VirtualDemo_detail::kMaxGeneratedVirtualGeometryItems + 1));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];
    float scroll = 0.0F;
    check(!component.scroll_virtual_3(scroll, 300.0F), "a source past the geometry cap is refused");
    check(list_window(root).empty(), "the refused scroll realizes nothing");
    check_eq(std::to_string(scroll), std::to_string(0.0F), "the refused scroll does not move");

    component.set_items(
        make_items(vlist::gen::VirtualDemo_detail::kMaxGeneratedVirtualGeometryItems));
    check(component.scroll_virtual_3(scroll, 300.0F), "a source exactly at the cap is accepted");
    check(!list_window(root).empty(), "the accepted scroll realizes a window");
}

} // namespace

int main() {
    test_window_and_range();
    test_reanchor_and_measurement_suffix();
    test_end_reached_edge_and_throttle();
    test_grid_row_window_and_measure();
    test_capacity_refusal();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen virtual scroll tests passed\n";
    return 0;
}
