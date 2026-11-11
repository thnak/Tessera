// SPDX-License-Identifier: MIT

// Runtime test for dynamic virtual geometry (virtualization-v1.md sections 6 and 10,
// docs/design/compiler.md section 11): the fixture's list estimate/gap come from state and its
// declared fixed extent from a prop, and the grid row height comes from state, so each case is a
// plain cell write followed by the next `scroll_virtual_<n>` call.
//
// The contract pinned here:
// - the generated re-seed path runs once per change, not once per frame (hash guard);
// - an `estimateSize`/`gap` change keeps every measurement and re-anchors so the item at the
//   viewport start keeps its viewport-relative position;
// - a changed declared fixed extent (`rowHeight`) drops the measurements it describes, because that
//   extent describes every item;
// - the re-anchor is clamped to `[0, total - viewport]` when the anchor no longer fits;
// - `onEndReached`'s edge/throttle state is neither reset nor fired by a geometry change.
//
// Expected extents and offsets come from a second `tessera::ui::virtualization::ItemGeometry` that
// receives the same measures and the same re-seed, so the test never re-implements prefix sums.

#include "VirtualDemo.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/item_geometry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// The re-seed path runs once per change: a per-frame scroll costs a hash pass, not a rebuild.
void test_reseed_once_per_change() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;
    float scroll = 900.0F;

    check(component.scroll_virtual_3(scroll, kViewport), "the first scroll applies");
    check(component.virtual_3_geometry_seeds_ == 1, "the first scroll seeds the geometry once");
    for (int frame = 0; frame < 5; ++frame) {
        check(component.scroll_virtual_3(scroll, kViewport), "a per-frame scroll applies");
    }
    check(component.virtual_3_geometry_seeds_ == 1,
          "an unchanged geometry is not re-seeded per frame");

    component.state_row_extent_ = 40;
    check(component.scroll_virtual_3(scroll, kViewport), "the changed-estimate scroll applies");
    check(component.virtual_3_geometry_seeds_ == 2, "a changed estimate re-seeds exactly once");
    check_eq(std::to_string(component.virtual_3_geometry_.estimate_main()), std::to_string(40.0F),
             "the geometry adopted the new estimate");
    check(component.scroll_virtual_3(scroll, kViewport), "the next frame applies");
    check(component.virtual_3_geometry_seeds_ == 2, "the refreshed hash suppresses a re-seed");

    // The declared fixed extent is a prop: it re-seeds too, and it never invalidates a measure
    // while `estimateSize` overrides it.
    component.prop_item_extent_ = 48;
    check(component.scroll_virtual_3(scroll, kViewport), "the changed-prop scroll applies");
    check(component.virtual_3_geometry_seeds_ == 3, "a changed geometry prop re-seeds once");
    check_eq(std::to_string(component.virtual_3_item_size_), std::to_string(48.0),
             "the re-seed publishes the prop value");
}

// An estimate change keeps every measurement and re-anchors the viewport-start item.
void test_estimate_change_keeps_measures() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;

    float scroll = 512.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the base scroll applies");
    const std::size_t measured = 8;
    const std::size_t slot = measured - component.virtual_3_first_;
    component.virtual_3_items_[slot].root->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 100.0F});
    check(component.scroll_virtual_3(scroll, kViewport), "the measured scroll applies");
    check(component.virtual_3_geometry_.is_measured(measured), "the measure is recorded");

    const std::size_t anchor = component.virtual_3_geometry_.index_at_offset(scroll);
    const float offset_in_anchor = scroll - component.virtual_3_geometry_.item_offset(anchor);
    const float total_before = component.virtual_3_geometry_.total_extent();

    virt::ItemGeometry expected{virt::ItemGeometry::Config{128, 32.0F, 4.0F}};
    check(expected.set_count(100), "the reference geometry accepts the source");
    check(expected.refine_measure(measured, 100.0F).changed, "the reference records the measure");

    component.state_row_extent_ = 40;
    check(component.scroll_virtual_3(scroll, kViewport), "the estimate-change scroll applies");
    check(component.virtual_3_geometry_.is_measured(measured),
          "an estimate change keeps the measure");
    check_eq(std::to_string(component.virtual_3_geometry_.item_main_size(measured)),
             std::to_string(100.0F), "the measured size survives the re-seed");
    check(component.virtual_3_geometry_.index_at_offset(scroll) == anchor,
          "the same first visible item stays at the viewport start");
    check_eq(std::to_string(scroll - component.virtual_3_geometry_.item_offset(anchor)),
             std::to_string(offset_in_anchor), "the anchor keeps its viewport-relative position");
    check(component.virtual_3_geometry_.total_extent() != total_before,
          "the unmeasured items moved to the new estimate");
    check(expected.reseed(40.0F, 4.0F), "the reference re-seeds too");
    check_eq(std::to_string(component.virtual_3_geometry_.total_extent()),
             std::to_string(expected.total_extent()),
             "the re-seeded extent matches an independent geometry");
    check_eq(std::to_string(component.virtual_3_geometry_.item_offset(anchor)),
             std::to_string(expected.item_offset(anchor)),
             "the re-seeded prefix sums match an independent geometry");
}

// A gap change keeps measurements too: a gap moves offsets, never an item's own size.
void test_gap_change_keeps_measures() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;

    float scroll = 1000.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the base scroll applies");
    const std::size_t measured = component.virtual_3_first_;
    component.virtual_3_items_[0].root->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 72.0F});
    check(component.scroll_virtual_3(scroll, kViewport), "the measured scroll applies");

    const std::size_t anchor = component.virtual_3_geometry_.index_at_offset(scroll);
    const float offset_in_anchor = scroll - component.virtual_3_geometry_.item_offset(anchor);
    component.state_row_gap_ = 10;
    check(component.scroll_virtual_3(scroll, kViewport), "the gap-change scroll applies");
    check_eq(std::to_string(component.virtual_3_geometry_.gap()), std::to_string(10.0F),
             "the geometry adopted the new gap");
    check(component.virtual_3_geometry_.is_measured(measured), "a gap change keeps the measure");
    check_eq(std::to_string(component.virtual_3_geometry_.item_main_size(measured)),
             std::to_string(72.0F), "the measured size survives a gap change");
    check_eq(std::to_string(scroll - component.virtual_3_geometry_.item_offset(anchor)),
             std::to_string(offset_in_anchor), "the anchor keeps its viewport position");
}

// A grid row-height change drops every row measurement: the declared extent describes every row
// (the documented invalidation rule), and the offset re-anchors with the new prefix sums.
void test_grid_row_height_change_drops_measures() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;

    float scroll = 1000.0F;
    check(component.scroll_virtual_6(scroll, kViewport), "the grid scroll applies");
    check(!component.virtual_6_geometry_.is_measured(19),
          "the first visible row starts as an estimate");
    component.virtual_6_items_[0].root->set_bounds(rhi::Rect{0.0F, 0.0F, 100.0F, 90.0F});
    check(component.scroll_virtual_6(scroll, kViewport), "the measured grid scroll applies");
    check(component.virtual_6_geometry_.is_measured(19), "the row is measured");
    check(component.virtual_6_geometry_.measured_count() == 1, "one row carries the measure");

    const std::size_t anchor = component.virtual_6_geometry_.index_at_offset(scroll);
    const float offset_in_anchor = scroll - component.virtual_6_geometry_.item_offset(anchor);
    component.state_grid_row_ = 80;
    check(component.update_virtual_geometry_6(scroll, kViewport),
          "the row-height change re-seeds (the public refresh path)");
    check(component.virtual_6_geometry_.measured_count() == 0,
          "a declared row-height change drops every row measurement");
    check_eq(std::to_string(component.virtual_6_geometry_.item_main_size(19)),
             std::to_string(80.0F), "every row takes the new row height");
    check_eq(std::to_string(component.virtual_6_geometry_.estimate_main()), std::to_string(80.0F),
             "the effective estimate follows the row height");
    check(component.virtual_6_geometry_.index_at_offset(scroll) == anchor,
          "the grid keeps its first visible row");
    check_eq(std::to_string(scroll - component.virtual_6_geometry_.item_offset(anchor)),
             std::to_string(offset_in_anchor), "the grid anchor keeps its viewport position");
    check(scroll >= 0.0F && scroll <= component.virtual_6_geometry_.total_extent() - kViewport,
          "the re-anchored grid offset stays inside the scrollable range");
    // A dropped measure is not a permanent forget: a node still laid out at its old extent reports
    // it again on the next measure pass (a measurement is a fact about the node).
    check(component.scroll_virtual_6(scroll, kViewport), "the post-reseed scroll applies");
    check_eq(std::to_string(component.virtual_6_geometry_.item_main_size(19)),
             std::to_string(90.0F), "a still-realized cell re-reports its laid-out extent");
}

// The clamped cases: a shrinking geometry re-anchors to `[0, total - viewport]`.
void test_geometry_change_clamp() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;

    float scroll = 0.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the zero scroll applies");
    component.state_row_extent_ = 64;
    check(component.scroll_virtual_3(scroll, kViewport), "the zero-scroll re-seed applies");
    check_eq(std::to_string(scroll), std::to_string(0.0F),
             "the first item keeps the viewport start at offset 0");

    component.state_row_extent_ = 32;
    check(component.scroll_virtual_3(scroll, kViewport), "the restored scroll applies");
    scroll = 3296.0F; // the maximum scroll of the 3596 px content
    check(component.scroll_virtual_3(scroll, kViewport), "the end scroll applies");
    const std::size_t anchor = component.virtual_3_geometry_.index_at_offset(scroll);
    component.state_row_extent_ = 20; // a smaller estimate shrinks the content to 2396 px
    check(component.scroll_virtual_3(scroll, kViewport), "the shrinking re-seed applies");
    const float max_scroll = component.virtual_3_geometry_.total_extent() - kViewport;
    check_eq(std::to_string(component.virtual_3_geometry_.total_extent()), std::to_string(2396.0F),
             "the shrunken content extent");
    check_eq(std::to_string(scroll), std::to_string(max_scroll),
             "the offset clamps to total - viewport");
    check(component.virtual_3_geometry_.item_offset(anchor) > max_scroll,
          "the clamp branch is the documented one: the old anchor no longer fits");
}

// The `onEndReached` edge/throttle state is not reset or fired by a geometry change.
void test_end_reached_state_survives_reseed() {
    gen::VirtualDemo component;
    component.state_items_ = make_items(100);
    ui::Node scene(1000);
    component.build(scene);
    constexpr float kViewport = 300.0F;
    constexpr float kMaxScroll = 3296.0F;

    float scroll = kMaxScroll;
    check(component.scroll_virtual_3(scroll, kViewport), "the end scroll applies");
    check(component.virtual_3_end_fires_ == 1, "the end fires once");
    const std::uint64_t throttled = component.virtual_3_end_throttled_;

    // An informational geometry change: the declared fixed extent, overridden by estimateSize.
    component.prop_item_extent_ = 48;
    check(component.scroll_virtual_3(scroll, kViewport), "the geometry-change scroll applies");
    check(component.virtual_3_geometry_seeds_ == 2, "the geometry did re-seed");
    check(component.virtual_3_end_fires_ == 1, "a geometry change does not fire onEndReached");
    check(component.virtual_3_end_throttled_ == throttled,
          "a geometry change does not consume the throttle");

    // A real change moves the content end, so the window leaves the end zone; the edge re-arms and
    // the next end-zone entry is still gated by distance (no reset, no wall clock).
    component.state_row_extent_ = 40;
    check(component.scroll_virtual_3(scroll, kViewport), "the estimate-change scroll applies");
    check(component.virtual_3_end_fires_ == 1, "the re-seed itself never fires");
    scroll = component.virtual_3_geometry_.total_extent() - kViewport;
    check(component.scroll_virtual_3(scroll, kViewport), "the new end scroll applies");
    check(component.virtual_3_end_fires_ <= 2, "the throttle still gates the end-zone entry");
}

} // namespace

int main() {
    test_reseed_once_per_change();
    test_estimate_change_keeps_measures();
    test_gap_change_keeps_measures();
    test_grid_row_height_change_drops_measures();
    test_geometry_change_clamp();
    test_end_reached_state_survives_reseed();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen virtual geometry runtime tests passed\n";
    return 0;
}
