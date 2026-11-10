// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_geometry.hpp"
#include "tessera/ui/virtual/scroll_anchor.hpp"

#include <cmath>
#include <iostream>

namespace {

using tessera::ui::virtualization::AnchorAdjustment;
using tessera::ui::virtualization::hash_virtual_item_id;
using tessera::ui::virtualization::ItemGeometry;
using tessera::ui::virtualization::reanchor_scroll;
using tessera::ui::virtualization::ScrollAnchor;
using tessera::ui::virtualization::VirtualItemId;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_anchor: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(value - expected) <= tolerance;
}

ItemGeometry make_geometry(std::size_t count) {
    ItemGeometry geometry(ItemGeometry::Config{64, 32.0f, 4.0f});
    if (!geometry.set_count(count)) {
        std::cerr << "virtual_anchor: setup set_count failed\n";
    }
    return geometry;
}

} // namespace

int main() {
    const VirtualItemId key{0x2A};
    const ScrollAnchor anchor{key, 28.0f};
    if (!expect(anchor.key == key && near(anchor.offset_in_item, 28.0f), "anchor fields") ||
        !expect(hash_virtual_item_id(key) == hash_virtual_item_id(key), "hash is stable") ||
        !expect(hash_virtual_item_id(VirtualItemId{1}) != hash_virtual_item_id(VirtualItemId{2}),
                "hash separates keys")) {
        return 1;
    }

    // Measuring an item before the anchor shifts the scroll by the same delta, so the anchor item
    // keeps its viewport-relative position while only the measured item's suffix moved.
    ItemGeometry geometry = make_geometry(10);
    const std::size_t anchor_index = geometry.index_at_offset(100.0f);
    const float offset_in_item = 100.0f - geometry.item_offset(anchor_index);
    if (!expect(anchor_index == 2 && near(offset_in_item, 28.0f), "anchor resolution")) {
        return 2;
    }
    ItemGeometry::MeasureChange change = geometry.refine_measure(1, 64.0f);
    AnchorAdjustment adjustment =
        reanchor_scroll(100.0f, 200.0f, geometry.total_extent(), anchor_index, 1, change.delta);
    if (!expect(adjustment.shifted, "scroll shifted for an earlier measure") ||
        !expect(!adjustment.clamped, "shift stayed inside the range") ||
        !expect(near(adjustment.scroll_offset, 132.0f), "scroll moved by the delta") ||
        !expect(near(geometry.item_offset(anchor_index) + offset_in_item, adjustment.scroll_offset),
                "anchor offset re-anchored") ||
        !expect(geometry.visible_range(adjustment.scroll_offset, 200.0f).first == anchor_index,
                "anchor is still the first visible item")) {
        return 3;
    }

    // Measuring the anchor item itself keeps the scroll offset (the anchor moved, not the prefix).
    geometry = make_geometry(10);
    change = geometry.refine_measure(2, 48.0f);
    adjustment = reanchor_scroll(100.0f, 200.0f, geometry.total_extent(), 2, 2, change.delta);
    if (!expect(!adjustment.shifted, "anchor measure does not shift the scroll") ||
        !expect(near(adjustment.scroll_offset, 100.0f), "anchor measure keeps the offset")) {
        return 4;
    }

    // Measuring an item after the anchor changes nothing before it.
    geometry = make_geometry(10);
    change = geometry.refine_measure(5, 80.0f);
    adjustment = reanchor_scroll(100.0f, 200.0f, geometry.total_extent(), 2, 5, change.delta);
    if (!expect(!adjustment.shifted, "later measure does not shift the scroll") ||
        !expect(near(adjustment.scroll_offset, 100.0f), "later measure keeps the offset")) {
        return 5;
    }

    // A shrinking earlier item shifts the scroll backwards by its negative delta.
    geometry = make_geometry(10);
    change = geometry.refine_measure(0, 16.0f);
    adjustment = reanchor_scroll(100.0f, 200.0f, geometry.total_extent(), 2, 0, change.delta);
    if (!expect(adjustment.shifted && near(adjustment.scroll_offset, 84.0f),
                "negative delta shifts backwards") ||
        !expect(!adjustment.clamped, "negative shift stays inside the range")) {
        return 6;
    }

    // The re-anchor result is clamped into the scrollable range.
    geometry = make_geometry(4);
    change = geometry.refine_measure(0, 0.0f);
    adjustment = reanchor_scroll(100.0f, 100.0f, geometry.total_extent(), 3, 0, change.delta);
    if (!expect(adjustment.shifted, "shrink shift applied") ||
        !expect(adjustment.clamped, "shrunken total clamps the scroll") ||
        !expect(near(adjustment.scroll_offset, 8.0f), "clamped to the new max scroll")) {
        return 7;
    }

    // Offset-only clamp paths: negative input and input past the end.
    adjustment = reanchor_scroll(-10.0f, 100.0f, 400.0f, 2, 2, 0.0f);
    if (!expect(!adjustment.shifted, "no delta never shifts") ||
        !expect(adjustment.clamped && near(adjustment.scroll_offset, 0.0f),
                "negative scroll clamps to zero")) {
        return 8;
    }
    adjustment = reanchor_scroll(1000.0f, 100.0f, 400.0f, 2, 2, 0.0f);
    if (!expect(adjustment.clamped && near(adjustment.scroll_offset, 300.0f),
                "scroll clamps to total minus viewport")) {
        return 9;
    }
    adjustment = reanchor_scroll(50.0f, 500.0f, 400.0f, 2, 2, 0.0f);
    if (!expect(adjustment.clamped && near(adjustment.scroll_offset, 0.0f),
                "viewport larger than content parks at zero")) {
        return 10;
    }

    return 0;
}
