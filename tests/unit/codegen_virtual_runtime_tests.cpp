// SPDX-License-Identifier: MIT

// Integration test for the VirtualList/VirtualGrid item-template codegen contract
// (ir-v1.md section 3.5, virtualization-v1.md sections 4 and 10) on the ADR-040 generated base:
// a scripted scroll drives the generated `apply_virtual_<n>(first, last)` with the range computed
// by the real `tessera::ui::virtualization::ItemGeometry` (the runtime owns the prefix sums; the
// test never re-implements them) and checks the realized nodes, their stable NodeId slots, keyed
// reuse across scrolls, the bounded window, the hash-compared content pass and the documented dirty
// domains.
//
// Everything is observed through the scene and the component's public accessors. The base's window
// bookkeeping (`virtual_N_count_`, `virtual_N_first_`, `virtual_N_items_`, the order hash) is
// implementation detail: the same facts are visible as the container's children, their
// absolute-index NodeIds and their painted text, which is exactly what a host can see.

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

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

void clear_all(ui::Node& node) {
    node.clear_dirty(ui::Dirty::All);
    for (const auto& child : node.children()) {
        clear_all(*child);
    }
}

std::string_view text_of(const ui::Node& node) {
    const auto& paint = node.paint_data();
    return paint ? std::string_view(paint->text) : std::string_view();
}

std::string item_text(const std::string& item, int count) {
    return item + " # " + std::to_string(count);
}

std::vector<std::string> make_items(std::size_t count) {
    std::vector<std::string> items;
    items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::string text = "row-";
        const std::string digits = std::to_string(index);
        text.append(3 - std::min<std::size_t>(3, digits.size()), '0');
        text += digits;
        items.push_back(std::move(text));
    }
    return items;
}

// The realized window is the container's child list, in window order.
const std::vector<std::unique_ptr<ui::Node>>& window_of(const ui::Node& container) {
    return container.children();
}

} // namespace

int main() {
    constexpr std::size_t kItemCount = 100;
    constexpr std::uint64_t kTemplateRootId = 4; // `node::4` in the fixture
    constexpr std::uint64_t kItemIdStride = 12;

    vlist::VirtualDemo component;
    const std::vector<std::string> items = make_items(kItemCount);
    component.set_items(items);

    ui::Node scene(1000);
    component.build(scene);
    check(scene.children().size() == 1, "the component builds exactly one root child");
    if (scene.children().empty()) {
        return 1;
    }
    ui::Node& root = *scene.children()[0];
    check(root.children().size() == 5,
          "the static tree has the header, collections, footer, button");
    if (root.children().size() != 5) {
        return 1;
    }
    ui::Node& list = *root.children()[1];
    ui::Node& grid = *root.children()[2];
    check_eq(list.element_kind(), "VirtualList", "the list element kind");
    check_eq(grid.element_kind(), "VirtualGrid", "the grid element kind");
    check(window_of(list).empty(), "no item is realized before the host applies a window");

    // --- 1. Scripted scroll: the runtime geometry owns the range; the generated code applies it.
    // --
    virt::ItemGeometry::Config config;
    config.capacity = 128;
    config.estimate_main = 32.0F; // the section 4 default the fixture's state seeds
    config.gap = 4.0F;
    virt::ItemGeometry geometry(config);
    check(geometry.set_count(items.size()), "the geometry accepts the data source count");

    constexpr float kViewport = 300.0F;
    const float offsets[] = {0.0F, 96.0F, 512.0F, 2048.0F, 3600.0F};
    for (const float scroll : offsets) {
        const virt::ItemGeometry::Range range = geometry.visible_range(scroll, kViewport);
        component.apply_virtual_3(range.first, range.last);
        check(window_of(list).size() == range.count(),
              "the container holds exactly the geometry's visible window");
        for (std::size_t slot = 0; slot < range.count(); ++slot) {
            const std::size_t index = range.first + slot;
            const ui::Node& item = *window_of(list)[slot];
            check(item.id() == static_cast<ui::NodeId>(kTemplateRootId + index * kItemIdStride),
                  "an item NodeId is the template id plus its absolute index times the stride");
            check_eq(text_of(*item.children()[0]), item_text(items[index], 0),
                     "the item text follows the data source at its absolute index");
        }
    }

    // --- 2. Keyed reuse: a key present in both windows keeps its node and its NodeId. -----------
    {
        const virt::ItemGeometry::Range first = geometry.visible_range(0.0F, kViewport);
        component.apply_virtual_3(first.first, first.last);
        std::vector<ui::Node*> pointers;
        for (const auto& item : window_of(list)) {
            pointers.push_back(item.get());
        }
        const virt::ItemGeometry::Range second = geometry.visible_range(96.0F, kViewport);
        component.apply_virtual_3(second.first, second.last);
        check(window_of(list).size() == second.count(), "the scrolled window keeps its size");
        // Index 2 was the first realized item at offset 0 and stays in the window at offset 96.
        check(window_of(list)[0].get() == pointers[2],
              "an item that stays in the window is reused, not rebuilt");
        check(window_of(list)[0]->id() ==
                  static_cast<ui::NodeId>(kTemplateRootId + 2 * kItemIdStride),
              "a reused item keeps its absolute-index NodeId");
    }

    // --- 3. Idempotent application: the same window touches nothing and marks nothing dirty. ----
    {
        const virt::ItemGeometry::Range range = geometry.visible_range(512.0F, kViewport);
        component.apply_virtual_3(range.first, range.last);
        clear_all(root);
        component.apply_virtual_3(range.first, range.last);
        check(root.dirty() == ui::Dirty::Clean,
              "re-applying the same window marks no dirty domain on the root");
        check(list.dirty() == ui::Dirty::Clean,
              "re-applying the same window marks no dirty domain on the container");
        check(window_of(list).size() == range.count(), "the window is unchanged");
    }

    // --- 4. Content-only change: same window, new state, Paint-only item repaint. ---------------
    {
        const virt::ItemGeometry::Range range = geometry.visible_range(512.0F, kViewport);
        component.apply_virtual_3(range.first, range.last);
        clear_all(root);
        component.set_count(1);
        component.apply_virtual_3(range.first, range.last);
        check(window_of(list).size() == range.count(), "a state change keeps the window");
        check_eq(text_of(*window_of(list)[0]->children()[0]), item_text(items[range.first], 1),
                 "the content pass repaints the item text");
        check(list.dirty() == ui::Dirty::Clean,
              "a content-only change does not restructure the container");
        check(ui::has_flag(window_of(list)[0]->children()[0]->dirty(), ui::Dirty::Paint) &&
                  !ui::has_flag(window_of(list)[0]->children()[0]->dirty(), ui::Dirty::Layout),
              "an item text repaint is Paint-only");
    }

    // --- 5. Unbounded window and clamped range.
    // ---------------------------------------------------
    {
        component.apply_virtual_3(0, kItemCount);
        check(window_of(list).size() == kItemCount,
              "a long visible range is realized in full, not clamped to 32 items");
        // Issue #4: 24 px rows on a 2160 px viewport plus the 200 px overscan realize ~99 rows.
        component.apply_virtual_3(0, 99);
        check(window_of(list).size() == 99,
              "a 4K-viewport window of 99 rows is realized without missing rows");
        component.apply_virtual_3(0, kItemCount);
        check(window_of(list).size() == kItemCount, "the container holds every realized item");
        component.apply_virtual_3(kItemCount - 4, kItemCount + 50);
        check(window_of(list).size() == 4,
              "a window past the data source clamps to the remaining items");
        component.apply_virtual_3(50, 50);
        check(window_of(list).empty(), "an empty window realizes nothing");
    }

    // --- 6. VirtualGrid reuses the same shape with its own geometry constants. ------------------
    {
        component.apply_virtual_6(2, 6);
        check(window_of(grid).size() == 4, "the grid realizes its requested window");
        check_eq(text_of(*window_of(grid)[0]->children()[0]), items[2],
                 "the grid item text follows its absolute index");
        check(window_of(grid)[0]->id() == static_cast<ui::NodeId>(7 + 2 * kItemIdStride),
              "the grid item NodeId uses the same stride rule");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen virtual runtime tests passed\n";
    return 0;
}
