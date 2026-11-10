// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/item_geometry.hpp"
#include "tessera/ui/virtual/virtual_item.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::ui::virtualization {

// Row-major grid geometry at row granularity (`docs/specs/virtualization-v1.md` section 4, Grid).
//
// A virtualized grid scrolls by rows: an item's identity is still its `VirtualItemId`, but the
// prefix sums are kept for rows because a row's main-axis extent is the maximum of its items'
// heights (or a fixed row height). The mapping is deterministic and index based:
// `row = index / columns`, `column = index % columns`, and the last row may be short.
//
// `Config::capacity` is a hard item-count cap allocated once in the constructor (the row arrays are
// sized for `ceil(capacity / columns)`), so `set_item_count`, `measure_item`, the offset/row
// queries and the visible-row windows never allocate. `measure_item` records the item's main size
// and folds it into its row as `max(measured items)`; re-measuring an item can shrink a row, and
// the row's `ItemGeometry` refinement shifts only the rows after it, exactly like the list path.
// Measurements of items that a `set_item_count` shrink removed are dropped, so growing the item
// count again starts those rows from the estimate instead of a stale height.
//
// `Config::fixed_row_extent > 0` selects the fixed-row-height variant: every row reports that
// extent and `measure_item` only records the item size (useful for uniform tiles).
class VirtualGridRows {
public:
    struct Config {
        std::size_t columns = 1; // clamped to at least one column
        std::size_t capacity = 0;
        float estimate_row = kItemEstimateMain; // row extent while no item is measured
        float gap = 0.0f;                       // main-axis gap between neighbouring rows
        float fixed_row_extent = 0.0f;          // > 0: all rows use this extent
    };

    // Half-open item window of one row. `count` is 0 for the clamped past-the-end row.
    struct Row {
        std::size_t index = 0;
        std::size_t first = 0;
        std::size_t count = 0;
    };

    explicit VirtualGridRows(Config config);

    // Item count -> row count (`ceil(count / columns)`). Returns false (changing nothing) when the
    // count exceeds the capacity.
    [[nodiscard]] bool set_item_count(std::size_t count) noexcept;

    [[nodiscard]] std::size_t item_count() const noexcept {
        return item_count_;
    }
    [[nodiscard]] std::size_t row_count() const noexcept {
        return rows_.item_count();
    }
    [[nodiscard]] std::size_t columns() const noexcept {
        return columns_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] bool fixed_row_extent() const noexcept {
        return config_.fixed_row_extent > 0.0f;
    }

    // Index -> position. `row_of` clamps to `row_count()`; `column_of` returns 0 past the end.
    [[nodiscard]] std::size_t row_of(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t column_of(std::size_t index) const noexcept;
    // Row at `row_index`; the past-the-end row is `{row_count(), item_count(), 0}`.
    [[nodiscard]] Row row(std::size_t row_index) const noexcept;

    // Records an item main size and folds it into its row. False for an out-of-range index. The
    // call is a no-op refinement (returns true) when the row extent does not change.
    [[nodiscard]] bool measure_item(std::size_t index, float main_size) noexcept;

    [[nodiscard]] float row_offset(std::size_t row_index) const noexcept;
    [[nodiscard]] float row_extent(std::size_t row_index) const noexcept;
    [[nodiscard]] float total_extent() const noexcept {
        return rows_.total_extent();
    }

    // Rows overlapping [scroll_offset, scroll_offset + viewport_extent), and the item window those
    // rows cover. Both are empty for an empty grid or a non-positive viewport.
    [[nodiscard]] ItemGeometry::Range visible_rows(float scroll_offset,
                                                   float viewport_extent) const noexcept;
    [[nodiscard]] ItemGeometry::Range visible_items(float scroll_offset,
                                                    float viewport_extent) const noexcept;
    [[nodiscard]] std::size_t row_at_offset(float offset) const noexcept;

    [[nodiscard]] const ItemGeometry& row_geometry() const noexcept {
        return rows_;
    }

private:
    [[nodiscard]] static std::size_t row_capacity(std::size_t capacity,
                                                  std::size_t columns) noexcept;
    void recompute_row(std::size_t row_index) noexcept;

    Config config_{};
    std::size_t columns_ = 1;
    std::size_t capacity_ = 0;
    std::size_t item_count_ = 0;
    ItemGeometry rows_;
    // Per-item main sizes and measured flags; the row extent is the max over its measured items.
    std::vector<float> item_main_;
    std::vector<std::uint8_t> item_measured_;
};

} // namespace tessera::ui::virtualization
