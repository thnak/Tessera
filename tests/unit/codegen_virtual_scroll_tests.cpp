// SPDX-License-Identifier: MIT

// End-to-end scripted scroll test for the generated collection scroll entry point
// (virtualization-v1.md sections 4, 6 and 10): `scroll_virtual_<n>` owns an `ItemGeometry` seeded
// from the emitted constants, applies the overscanned realized window through `apply_virtual_<n>`,
// refines the prefix sums from realized item bounds and re-anchors the caller's offset, and
// dispatches the collection events (`onEndReached` edge/throttle state lives in generated members).
//
// The expected windows are computed with a second `tessera::ui::virtualization::ItemGeometry` that
// receives the same measurements, so the test never re-implements prefix sums.

#include "VirtualDemo.hpp"
#include "tessera/ui/scene/node.hpp"
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
namespace gen = vlist::gen;

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

std::size_t realized_items(const gen::VirtualDemo& component) {
    return component.virtual_3_->children().size();
}

// 1. The generated driver realizes exactly the overscanned window of an independent geometry.
void test_window_and_range() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);

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
        check_eq(std::to_string(component.virtual_3_first_), std::to_string(window.range.first),
                 "the realized window start");
        check_eq(std::to_string(component.virtual_3_count_), std::to_string(window.range.count()),
                 "the realized window count");
        check_eq(std::to_string(realized_items(component)), std::to_string(window.range.count()),
                 "the container holds the realized window");
        for (std::size_t slot = 0; slot < component.virtual_3_count_; ++slot) {
            const std::size_t index = component.virtual_3_first_ + slot;
            const ui::Node& item = *component.virtual_3_items_[slot].root;
            check(item.id() == static_cast<ui::NodeId>(4 + index * 12),
                  "an item keeps its absolute-index NodeId");
        }
    }
    // The geometry is seeded from the emitted constants, not from the host's numbers.
    check(component.virtual_3_geometry_.estimate_main() == 32.0F,
          "the geometry estimate is the emitted estimateSize");
    check(component.virtual_3_geometry_.gap() == 4.0F, "the geometry gap is the emitted gap");
    check_eq(std::to_string(static_cast<int>(component.virtual_3_geometry_.total_extent())),
             std::to_string(3596), "the unmeasured extent follows the 32 px estimate");
}

// 2. A measured item before the viewport anchor shifts the offset and only the suffix moves.
void test_reanchor_and_measurement_suffix() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;

    virt::ItemGeometry expected{virt::ItemGeometry::Config{128, 32.0F, 4.0F}};
    check(expected.set_count(100), "the reference geometry accepts 100 items");

    float scroll = 512.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the first scroll applies");
    check_eq(std::to_string(scroll), std::to_string(512.0F), "the first scroll does not shift");

    // Item 8 is realized (the window starts at its offset 288) and sits before the viewport anchor
    // (item 14 at offset 512): a 100 px measure shifts the scroll by its 68 px delta.
    const std::size_t measured = 8;
    check(component.virtual_3_first_ <= measured, "the item under measure is realized");
    const std::size_t slot = measured - component.virtual_3_first_;
    component.virtual_3_items_[slot].root->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 100.0F});
    const float before_offset_after = expected.item_offset(9);
    const virt::ItemGeometry::MeasureChange expected_change =
        expected.refine_measure(measured, 100.0F);
    check(expected_change.changed, "the reference geometry recorded the measure");

    scroll = 512.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the measured scroll applies");
    check_eq(std::to_string(scroll), std::to_string(580.0F),
             "a measure before the anchor shifts the scroll by its delta");
    check_eq(std::to_string(component.virtual_3_geometry_.item_main_size(measured)),
             std::to_string(100.0F), "the geometry stores the reported measure");
    check(component.virtual_3_geometry_.is_measured(measured), "the item is marked measured");
    check_eq(std::to_string(component.virtual_3_geometry_.item_offset(measured + 1)),
             std::to_string(before_offset_after + 68.0F),
             "only the suffix after the measured item shifts");
    check_eq(std::to_string(static_cast<int>(component.virtual_3_geometry_.total_extent())),
             std::to_string(3664), "the total extent grows by the measure delta");

    // The same measure on the next scroll is a no-op: the geometry is already refined.
    const std::size_t count_before = component.virtual_3_count_;
    check(component.scroll_virtual_3(scroll, kViewport), "the second measured scroll applies");
    check_eq(std::to_string(scroll), std::to_string(580.0F), "a repeated measure does not shift");
    check(component.virtual_3_count_ == count_before, "the window is stable");

    // A measure after the anchor changes the suffix but never the scroll (section 6).
    const std::size_t after = 20;
    const std::size_t after_slot = after - component.virtual_3_first_;
    check(after_slot < component.virtual_3_count_, "the item after the anchor is realized");
    component.virtual_3_items_[after_slot].root->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 60.0F});
    const float offset_before = component.virtual_3_geometry_.item_offset(after);
    const float suffix_before = component.virtual_3_geometry_.item_offset(after + 1);
    check(component.scroll_virtual_3(scroll, kViewport), "the late measure applies");
    check_eq(std::to_string(scroll), std::to_string(580.0F),
             "a measure after the anchor does not move the scroll");
    check_eq(std::to_string(component.virtual_3_geometry_.item_offset(after)),
             std::to_string(offset_before), "the measured item keeps its offset");
    check_eq(std::to_string(component.virtual_3_geometry_.item_offset(after + 1)),
             std::to_string(suffix_before + 28.0F), "the suffix after it shifts by the delta");
}

// 3. `onEndReached` is edge-triggered and distance-throttled (generated state members).
void test_end_reached_edge_and_throttle() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;
    constexpr float kMaxScroll = 3296.0F; // total 3596 - viewport 300

    float scroll = 0.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "scroll 0 applies");
    check(component.virtual_3_end_fires_ == 0, "the first screen is not at the end");

    scroll = kMaxScroll;
    check(component.scroll_virtual_3(scroll, kViewport), "the end scroll applies");
    check(component.virtual_3_end_fires_ == 1, "reaching the end fires onEndReached once");
    check(component.state_count_ == 1, "the generated onEndReached handler ran");
    check(component.virtual_3_end_throttled_ == 0, "the first fire is never throttled");

    check(component.scroll_virtual_3(scroll, kViewport), "the repeated end scroll applies");
    check(component.virtual_3_end_fires_ == 1, "a repeated scroll at the end does not re-fire");

    scroll = 3150.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the in-zone scroll applies");
    check(component.virtual_3_end_fires_ == 1, "staying in the end zone does not re-fire");

    scroll = 3000.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "leaving the end zone applies");
    scroll = 3150.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the near re-entry applies");
    check(component.virtual_3_end_fires_ == 1,
          "a re-entry within the throttle distance is suppressed");
    check(component.virtual_3_end_throttled_ == 1, "the suppressed entry is counted");

    scroll = 2700.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the far scroll applies");
    scroll = 3080.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the far re-entry applies");
    check(component.virtual_3_end_fires_ == 2,
          "a re-entry beyond the throttle distance fires again");
    check(component.virtual_3_end_throttled_ == 1, "no extra suppression is counted");
}

// 4. VirtualGrid maps the row window back to an item window and refines row extents.
void test_grid_row_window_and_measure() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;

    virt::ItemGeometry rows{virt::ItemGeometry::Config{128, 40.0F, 2.0F}};
    check(rows.set_count(50), "the reference grid geometry accepts 50 rows");

    float scroll = 0.0F;
    check(component.scroll_virtual_6(scroll, kViewport), "the grid scroll 0 applies");
    const Window first = window_of(rows, scroll, kViewport);
    check_eq(std::to_string(component.virtual_6_first_), std::to_string(first.range.first * 2),
             "the grid realizes whole rows");
    check_eq(std::to_string(component.virtual_6_count_),
             std::to_string((first.range.last - first.range.first) * 2),
             "the grid realized count is the row window times the column count");

    scroll = 1000.0F;
    check(component.scroll_virtual_6(scroll, kViewport), "the grid scroll 1000 applies");
    check_eq(std::to_string(component.virtual_6_first_), std::to_string(38),
             "the grid window starts at the first item of the first visible row");
    check_eq(std::to_string(component.virtual_6_count_),
             std::to_string(gen::kMaxGeneratedVirtualItems),
             "a row window wider than the item table is clamped by apply_virtual_<n>");

    // A cell reports 90 px: row 19 grows from the 40 px row height and, since row 19 is before the
    // viewport anchor row, the offset shifts by the 50 px delta.
    const std::size_t cell_slot = 0;
    component.virtual_6_items_[cell_slot].root->set_bounds(rhi::Rect{0.0F, 0.0F, 100.0F, 90.0F});
    scroll = 1000.0F;
    check(component.scroll_virtual_6(scroll, kViewport), "the measured grid scroll applies");
    check_eq(std::to_string(scroll), std::to_string(1050.0F),
             "a measured row before the anchor shifts the scroll");
    check_eq(std::to_string(component.virtual_6_geometry_.item_main_size(19)),
             std::to_string(90.0F), "the row extent follows the measured cell");
    check_eq(std::to_string(static_cast<int>(component.virtual_6_geometry_.total_extent())),
             std::to_string(2148), "the grid extent grows by the row delta");
}

// 5. A data source past the generated geometry cap is refused loudly, and nothing is realized.
void test_capacity_refusal() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(gen::kMaxGeneratedVirtualGeometryItems + 1);
    ui::Node scene(1000);
    component.build(scene);
    float scroll = 0.0F;
    check(!component.scroll_virtual_3(scroll, 300.0F), "a source past the geometry cap is refused");
    check(component.virtual_3_count_ == 0, "the refused scroll realizes nothing");
    check_eq(std::to_string(scroll), std::to_string(0.0F), "the refused scroll does not move");

    component.state_items_ = make_items(gen::kMaxGeneratedVirtualGeometryItems);
    check(component.scroll_virtual_3(scroll, 300.0F), "a source exactly at the cap is accepted");
    check(component.virtual_3_count_ > 0, "the accepted scroll realizes a window");
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
