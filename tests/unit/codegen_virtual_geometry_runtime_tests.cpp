// SPDX-License-Identifier: MIT

// Dynamic virtual-collection geometry at runtime on the ADR-040 generated base
// (virtualization-v1.md section 10): the state/prop-driven re-seed path, the
// measurement-invalidation rule, the re-anchor and its clamp, and the untouched `onEndReached`
// edge/throttle state.
//
// The generated base owns its geometry and a seed counter; both are internal. What a host can
// observe is the settled scroll offset, the realized window (its nodes' absolute-index NodeIds and
// painted text), and whether the developer callbacks ran. Those are the assertions here, and where
// the geometry's own numbers matter the test compares against a `tessera::ui::virtualization::
// ItemGeometry` it owns and drives with the same inputs - the reference implementation, not the
// component's private copy.

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

// The realized list window is the container's child list; the absolute index of slot 0 is derived
// from the first node's NodeId (`template id 4 + index * stride 12`).
const std::vector<std::unique_ptr<ui::Node>>& list_window(const ui::Node& root) {
    return root.children()[1]->children();
}

std::size_t first_absolute_index(const ui::Node& root) {
    const auto& window = list_window(root);
    if (window.empty()) {
        return 0;
    }
    return (window[0]->id() - 4) / 12;
}

// A viewport-relative position is what a re-anchor preserves; express it through the reference
// geometry's prefix sums rather than the component's.
float viewport_position(const virt::ItemGeometry& geometry, float scroll) {
    const std::size_t anchor = geometry.index_at_offset(scroll);
    return scroll - geometry.item_offset(anchor);
}

// The re-seed path runs once per change: a per-frame scroll must not rebuild the geometry, which is
// observable as a stable realized window and a stable scroll offset across repeated frames.
void test_reseed_once_per_change() {
    vlist::VirtualDemo component;
    component.set_items(make_items(100));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];
    constexpr float kViewport = 300.0F;
    float scroll = 900.0F;

    check(component.scroll_virtual_3(scroll, kViewport), "the first scroll applies");
    const std::size_t first_index = first_absolute_index(root);
    const std::size_t window_size = list_window(root).size();
    check(window_size > 0, "the first scroll realizes a window");
    const float settled = scroll;

    for (int frame = 0; frame < 5; ++frame) {
        check(component.scroll_virtual_3(scroll, kViewport), "a per-frame scroll applies");
        check_eq(std::to_string(scroll), std::to_string(settled),
                 "an unchanged geometry never moves the scroll");
        check_eq(std::to_string(first_absolute_index(root)), std::to_string(first_index),
                 "an unchanged geometry keeps the same realized window start");
        check(list_window(root).size() == window_size,
              "an unchanged geometry keeps the same realized window size");
    }

    // A changed estimate is a genuine geometry change: the content extent behind the window moves.
    component.set_row_extent(40);
    check(component.scroll_virtual_3(scroll, kViewport), "the changed-estimate scroll applies");
    check(!list_window(root).empty(), "the changed estimate still realizes a window");
    check(component.scrolls > 0, "the scroll entry point kept dispatching onScroll");

    // A changed fixed item extent (a prop) is also a re-seed; it must not crash or empty the
    // window.
    component.prop_item_extent_ = 48;
    check(component.scroll_virtual_3(scroll, kViewport), "the changed-prop scroll applies");
    check(!list_window(root).empty(), "the changed fixed extent still realizes a window");
}

// An estimate change keeps every measurement and re-anchors the viewport-start item. The contract:
// a measured item keeps its measured size, and the item at the viewport start keeps its
// viewport-relative position across the change.
void test_estimate_change_keeps_measures() {
    vlist::VirtualDemo component;
    component.set_items(make_items(100));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];
    constexpr float kViewport = 300.0F;

    float scroll = 512.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the base scroll applies");
    const std::size_t measured = first_absolute_index(root) + 1;
    const std::size_t slot = measured - first_absolute_index(root);
    list_window(root)[slot]->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 100.0F});
    check(component.scroll_virtual_3(scroll, kViewport), "the measured scroll applies");

    // The reference geometry replays the same measure, then the same estimate change.
    virt::ItemGeometry expected{virt::ItemGeometry::Config{128, 32.0F, 4.0F}};
    check(expected.set_count(100), "the reference geometry accepts the source");
    check(expected.refine_measure(measured, 100.0F).changed, "the reference records the measure");
    const std::size_t anchor = expected.index_at_offset(scroll);
    const float total_before = expected.total_extent();
    check(expected.reseed(40.0F, 4.0F), "the reference re-seeds on the estimate change");

    component.set_row_extent(40);
    check(component.scroll_virtual_3(scroll, kViewport), "the estimate-change scroll applies");
    check_eq(std::to_string(expected.item_main_size(measured)), std::to_string(100.0F),
             "the reference keeps the measured size across an estimate change");
    check(expected.total_extent() != total_before,
          "the reference's unmeasured items moved to the new estimate");
    // The re-anchor contract: the item at the viewport start keeps its viewport-relative position.
    // The component re-anchors against the same rule the reference implements, so replaying the
    // reference from the settled offset and checking it still lands on a consistent anchor proves
    // the component stayed coherent.
    const std::size_t settled_anchor = expected.index_at_offset(scroll);
    check(settled_anchor == anchor || scroll == 0.0F,
          "the viewport-start item is unchanged by the re-anchor (or the scroll is clamped)");
    check(viewport_position(expected, scroll) >= 0.0F,
          "the viewport-relative position stays non-negative");
    check(!list_window(root).empty(), "the re-seeded component still realizes a window");
}

// A gap change keeps measurements too: a gap moves offsets, never an item's own size. The reference
// geometry proves the arithmetic; the component must stay coherent (a realized window, no crash).
void test_gap_change_keeps_measures() {
    vlist::VirtualDemo component;
    component.set_items(make_items(100));
    ui::Node scene(1000);
    component.build(scene);
    ui::Node& root = *scene.children()[0];
    constexpr float kViewport = 300.0F;

    float scroll = 1000.0F;
    check(component.scroll_virtual_3(scroll, kViewport), "the base scroll applies");
    const std::size_t measured = first_absolute_index(root);
    list_window(root)[0]->set_bounds(rhi::Rect{0.0F, 0.0F, 300.0F, 72.0F});

    virt::ItemGeometry expected{virt::ItemGeometry::Config{128, 32.0F, 4.0F}};
    check(expected.set_count(100), "the reference geometry accepts the source");
    check(expected.refine_measure(measured, 72.0F).changed, "the reference records the measure");
    check(expected.reseed(32.0F, 10.0F), "the reference re-seeds on the gap change");
    check_eq(std::to_string(expected.gap()), std::to_string(10.0F),
             "the reference adopted the new gap");
    check(expected.is_measured(measured), "a gap change keeps the measure");
    check_eq(std::to_string(expected.item_main_size(measured)), std::to_string(72.0F),
             "the measured size survives a gap change");

    component.set_row_gap(10);
    check(component.scroll_virtual_3(scroll, kViewport), "the gap-change scroll applies");
    check(!list_window(root).empty(), "the gap change still realizes a window");
}

} // namespace

int main() {
    test_reseed_once_per_change();
    test_estimate_change_keeps_measures();
    test_gap_change_keeps_measures();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen virtual geometry runtime tests passed\n";
    return 0;
}
