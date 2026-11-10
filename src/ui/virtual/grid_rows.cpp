// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/grid_rows.hpp"

#include <algorithm>

namespace tessera::ui::virtualization {

namespace {

[[nodiscard]] std::size_t clamp_columns(std::size_t columns) noexcept {
    return std::max<std::size_t>(columns, 1);
}

[[nodiscard]] float row_estimate(const VirtualGridRows::Config& config) noexcept {
    const float fixed = std::max(config.fixed_row_extent, 0.0f);
    return fixed > 0.0f ? fixed : std::max(config.estimate_row, 0.0f);
}

} // namespace

VirtualGridRows::VirtualGridRows(Config config)
    : config_(config),
      columns_(clamp_columns(config.columns)),
      capacity_(config.capacity),
      rows_(ItemGeometry::Config{.capacity = row_capacity(config.capacity, columns_),
                                 .estimate_main = row_estimate(config),
                                 .gap = std::max(config.gap, 0.0f)}),
      item_main_(config.capacity, 0.0f),
      item_measured_(config.capacity, 0) {
    config_.fixed_row_extent = std::max(config_.fixed_row_extent, 0.0f);
    config_.estimate_row = std::max(config_.estimate_row, 0.0f);
    config_.gap = std::max(config_.gap, 0.0f);
}

std::size_t VirtualGridRows::row_capacity(std::size_t capacity, std::size_t columns) noexcept {
    const std::size_t safe_columns = clamp_columns(columns);
    return capacity / safe_columns + (capacity % safe_columns != 0 ? 1 : 0);
}

bool VirtualGridRows::set_item_count(std::size_t count) noexcept {
    if (count > capacity_) {
        return false;
    }
    // Drop the measurements of items that disappear. A later grow then starts those rows from the
    // estimate instead of resurrecting a height that belonged to a removed item.
    for (std::size_t index = count; index < item_count_; ++index) {
        item_main_[index] = 0.0f;
        item_measured_[index] = 0;
    }
    const std::size_t next_rows = count == 0 ? 0 : row_capacity(count, columns_);
    if (!rows_.set_count(next_rows)) {
        return false;
    }
    item_count_ = count;
    // The last row can lose or gain items inside the same row index, so every surviving row is
    // re-derived. This runs on a data change (never on scroll), so O(item_count) is acceptable.
    for (std::size_t row_index = 0; row_index < next_rows; ++row_index) {
        recompute_row(row_index);
    }
    return true;
}

std::size_t VirtualGridRows::row_of(std::size_t index) const noexcept {
    if (item_count_ == 0) {
        return 0;
    }
    return std::min(index / columns_, rows_.item_count());
}

std::size_t VirtualGridRows::column_of(std::size_t index) const noexcept {
    return index >= item_count_ ? 0 : index % columns_;
}

VirtualGridRows::Row VirtualGridRows::row(std::size_t row_index) const noexcept {
    const std::size_t row_count_value = rows_.item_count();
    if (row_index >= row_count_value) {
        return Row{row_count_value, item_count_, 0};
    }
    const std::size_t first = row_index * columns_;
    return Row{row_index, first, std::min(columns_, item_count_ - first)};
}

bool VirtualGridRows::measure_item(std::size_t index, float main_size) noexcept {
    if (index >= item_count_) {
        return false;
    }
    item_main_[index] = std::max(main_size, 0.0f);
    item_measured_[index] = 1;
    recompute_row(row_of(index));
    return true;
}

void VirtualGridRows::recompute_row(std::size_t row_index) noexcept {
    if (row_index >= rows_.item_count()) {
        return;
    }
    float extent = row_estimate(config_);
    if (config_.fixed_row_extent <= 0.0f) {
        const Row range = row(row_index);
        for (std::size_t index = range.first; index < range.first + range.count; ++index) {
            if (item_measured_[index] != 0) {
                extent = std::max(extent, item_main_[index]);
            }
        }
    }
    (void)rows_.refine_measure(row_index, extent);
}

float VirtualGridRows::row_offset(std::size_t row_index) const noexcept {
    return rows_.item_offset(std::min(row_index, rows_.item_count()));
}

float VirtualGridRows::row_extent(std::size_t row_index) const noexcept {
    return rows_.item_main_size(row_index);
}

ItemGeometry::Range VirtualGridRows::visible_rows(float scroll_offset,
                                                  float viewport_extent) const noexcept {
    return rows_.visible_range(scroll_offset, viewport_extent);
}

ItemGeometry::Range VirtualGridRows::visible_items(float scroll_offset,
                                                   float viewport_extent) const noexcept {
    const ItemGeometry::Range visible = visible_rows(scroll_offset, viewport_extent);
    if (visible.empty()) {
        return ItemGeometry::Range{0, 0};
    }
    const Row first_row = row(visible.first);
    const Row last_row = row(visible.last - 1);
    return ItemGeometry::Range{first_row.first,
                               std::min(last_row.first + last_row.count, item_count_)};
}

std::size_t VirtualGridRows::row_at_offset(float offset) const noexcept {
    return rows_.index_at_offset(offset);
}

} // namespace tessera::ui::virtualization
