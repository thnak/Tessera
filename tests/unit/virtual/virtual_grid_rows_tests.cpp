// SPDX-License-Identifier: MIT
//
// Grid row math over `ItemGeometry` (`docs/specs/virtualization-v1.md` section 4, Grid): row-major
// index mapping, max-item-height row extents, fixed row heights, gap/offset prefix sums, visible
// row/item windows, capacity refusal, and dropping measurements of removed items.

#include "tessera/ui/virtual/grid_rows.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <utility>

namespace {

using tessera::ui::virtualization::ItemGeometry;
using tessera::ui::virtualization::VirtualGridRows;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_grid_rows: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-3f) {
    return std::fabs(value - expected) <= tolerance;
}

VirtualGridRows::Config grid_config(std::size_t columns, std::size_t capacity) {
    VirtualGridRows::Config config;
    config.columns = columns;
    config.capacity = capacity;
    config.estimate_row = 32.0f;
    config.gap = 4.0f;
    return config;
}

} // namespace

int main() {
    // Row-major mapping, including the short last row and the clamped past-the-end row.
    VirtualGridRows grid(grid_config(3, 32));
    if (!expect(grid.set_item_count(10), "set ten items") ||
        !expect(grid.row_count() == 4, "ten items over three columns is four rows") ||
        !expect(grid.columns() == 3 && grid.item_count() == 10 && grid.capacity() == 32,
                "config is reported")) {
        return 1;
    }
    if (!expect(grid.row_of(0) == 0 && grid.row_of(2) == 0 && grid.row_of(3) == 1 &&
                    grid.row_of(9) == 3,
                "row = index / columns") ||
        !expect(grid.column_of(0) == 0 && grid.column_of(2) == 2 && grid.column_of(3) == 0 &&
                    grid.column_of(9) == 0,
                "column = index % columns") ||
        !expect(grid.row_of(1000) == grid.row_count(), "row_of clamps to the row count") ||
        !expect(grid.column_of(10) == 0, "column_of past the end is zero")) {
        return 2;
    }
    VirtualGridRows::Row first_row = grid.row(0);
    VirtualGridRows::Row last_row = grid.row(3);
    VirtualGridRows::Row past_end = grid.row(4);
    if (!expect(first_row.index == 0 && first_row.first == 0 && first_row.count == 3,
                "full first row") ||
        !expect(last_row.index == 3 && last_row.first == 9 && last_row.count == 1,
                "short last row") ||
        !expect(past_end.index == 4 && past_end.first == 10 && past_end.count == 0,
                "past-the-end row is empty")) {
        return 3;
    }

    // Unmeasured rows use the estimate: offsets are (extent + gap) per row, the total drops the
    // trailing gap.
    if (!expect(near(grid.row_offset(0), 0.0f) && near(grid.row_offset(1), 36.0f),
                "row offsets use the estimate and the gap") ||
        !expect(near(grid.row_offset(3), 108.0f), "third row offset") ||
        !expect(near(grid.total_extent(), 140.0f), "total is rows * extent + gap between rows") ||
        !expect(near(grid.row_extent(2), 32.0f), "unmeasured row extent")) {
        return 4;
    }

    // A row extent is the max of its measured items; re-measuring smaller shrinks the row and the
    // suffix shifts back, while rows before it keep their offset.
    if (!expect(grid.measure_item(0, 48.0f) && near(grid.row_extent(0), 48.0f),
                "first measured item sizes the row") ||
        !expect(grid.measure_item(1, 40.0f) && near(grid.row_extent(0), 48.0f),
                "a smaller item does not shrink the row") ||
        !expect(grid.measure_item(2, 60.0f) && near(grid.row_extent(0), 60.0f),
                "the tallest item decides the row") ||
        !expect(near(grid.row_offset(1), 64.0f), "row 1 shifts by the row-0 delta") ||
        !expect(near(grid.total_extent(), 168.0f) && near(grid.row_offset(2), 100.0f),
                "suffix shift after the measured row") ||
        !expect(grid.measure_item(2, 20.0f) && near(grid.row_extent(0), 48.0f),
                "re-measuring the tallest item smaller shrinks the row") ||
        !expect(near(grid.row_offset(1), 52.0f) && near(grid.total_extent(), 156.0f),
                "shrink shifts the suffix back") ||
        !expect(!grid.measure_item(10, 10.0f), "measuring a past-the-end item fails") ||
        !expect(near(grid.row_extent(1), 32.0f), "another row is unaffected")) {
        return 5;
    }
    // Rows are independent: measuring row 2 leaves row 0 and the rows before row 2 untouched.
    if (!expect(grid.measure_item(4, 100.0f) && near(grid.row_extent(1), 100.0f),
                "row 1 takes its own item height") ||
        !expect(near(grid.row_extent(0), 48.0f) && near(grid.row_offset(0), 0.0f),
                "row 0 is untouched") ||
        !expect(near(grid.row_offset(2), 156.0f), "row 2 starts after rows 0 and 1")) {
        return 6;
    }

    // Fixed row height: the configured extent wins over any item measurement.
    VirtualGridRows::Config fixed_config = grid_config(2, 8);
    fixed_config.fixed_row_extent = 24.0f;
    fixed_config.gap = 0.0f;
    VirtualGridRows fixed(fixed_config);
    if (!expect(fixed.fixed_row_extent(), "fixed-row mode is reported") ||
        !expect(fixed.set_item_count(8) && fixed.row_count() == 4, "four rows of two") ||
        !expect(near(fixed.total_extent(), 96.0f), "fixed rows ignore the estimate") ||
        !expect(fixed.measure_item(0, 999.0f) && near(fixed.row_extent(0), 24.0f),
                "an item measurement never overrides a fixed row height")) {
        return 7;
    }

    // Visible rows and the item window those rows cover.
    VirtualGridRows::Config windowed_config = grid_config(2, 16);
    windowed_config.estimate_row = 40.0f;
    windowed_config.gap = 0.0f;
    VirtualGridRows windowed(windowed_config);
    if (!expect(windowed.set_item_count(16) && windowed.row_count() == 8,
                "eight rows of two items")) {
        return 8;
    }
    ItemGeometry::Range rows = windowed.visible_rows(0.0f, 80.0f);
    ItemGeometry::Range items = windowed.visible_items(0.0f, 80.0f);
    if (!expect(rows.first == 0 && rows.last == 2, "top window covers two rows") ||
        !expect(items.first == 0 && items.last == 4, "top window covers four items") ||
        !expect(windowed.row_at_offset(0.0f) == 0 && windowed.row_at_offset(79.0f) == 1,
                "row_at_offset follows the row prefix sums")) {
        return 9;
    }
    rows = windowed.visible_rows(40.0f, 80.0f);
    items = windowed.visible_items(40.0f, 80.0f);
    if (!expect(rows.first == 1 && rows.last == 3, "scrolled rows") ||
        !expect(items.first == 2 && items.last == 6, "scrolled items follow the rows") ||
        !expect(windowed.row_at_offset(120.0f) == 3, "scrolled row_at_offset")) {
        return 10;
    }
    if (!expect(windowed.visible_rows(0.0f, 0.0f).empty(), "zero viewport has no rows") ||
        !expect(windowed.visible_items(0.0f, 0.0f).empty(), "zero viewport has no items")) {
        return 11;
    }

    // Capacity is a hard cap and a refusal leaves the grid unchanged.
    VirtualGridRows capped(grid_config(2, 4));
    if (!expect(!capped.set_item_count(5), "count past capacity is refused") ||
        !expect(capped.item_count() == 0 && capped.row_count() == 0, "refusal changes nothing") ||
        !expect(capped.set_item_count(4) && capped.row_count() == 2, "count at capacity")) {
        return 12;
    }

    // A shrink drops the removed items' measurements, so growing again starts from the estimate.
    VirtualGridRows::Config resized_config = grid_config(2, 16);
    resized_config.estimate_row = 10.0f;
    resized_config.gap = 0.0f;
    VirtualGridRows resized(resized_config);
    if (!expect(resized.set_item_count(8) && resized.row_count() == 4, "seed rows") ||
        !expect(resized.measure_item(6, 100.0f) && near(resized.row_extent(3), 100.0f),
                "tall item sizes the last row") ||
        !expect(near(resized.total_extent(), 130.0f), "total with the tall row") ||
        !expect(resized.set_item_count(2) && resized.row_count() == 1, "shrink to a single row") ||
        !expect(near(resized.total_extent(), 10.0f), "shrink drops the removed rows") ||
        !expect(resized.set_item_count(8) && resized.row_count() == 4, "grow back to four rows") ||
        !expect(near(resized.row_extent(3), 10.0f),
                "the removed item's measurement is not resurrected") ||
        !expect(near(resized.total_extent(), 40.0f), "grown rows start from the estimate")) {
        return 13;
    }

    // Zero columns clamps to one, and an empty grid has no rows, no extent and no window.
    VirtualGridRows single(grid_config(0, 4));
    if (!expect(single.columns() == 1, "zero columns clamp to one") ||
        !expect(single.set_item_count(3) && single.row_count() == 3, "one item per row") ||
        !expect(single.set_item_count(0) && single.row_count() == 0, "empty grid has no rows") ||
        !expect(near(single.total_extent(), 0.0f) && single.visible_rows(0.0f, 100.0f).empty(),
                "empty grid has no extent or window") ||
        !expect(single.row(0).count == 0 && single.row_at_offset(0.0f) == 0,
                "empty grid rows are empty")) {
        return 14;
    }

    // Two identical constructions produce identical geometry (no clock, RNG, or pointer order).
    const auto snapshot = []() {
        VirtualGridRows run(grid_config(3, 32));
        (void)run.set_item_count(10);
        (void)run.measure_item(1, 55.0f);
        (void)run.measure_item(7, 70.0f);
        return std::pair{run.total_extent(), run.row_offset(2)};
    };
    if (!expect(snapshot() == snapshot(), "grid geometry is deterministic")) {
        return 15;
    }

    return 0;
}
