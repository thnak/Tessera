// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_geometry.hpp"

#include <algorithm>
#include <bit>

namespace tessera::ui::virtualization {

namespace {

// Lowbit of an unsigned index; the Fenwick tree walks indices by this stride.
[[nodiscard]] constexpr std::size_t lowbit(std::size_t index) noexcept {
    return index & (~index + 1);
}

} // namespace

ItemGeometry::ItemGeometry(Config config)
    : capacity_(config.capacity),
      estimate_(std::max(config.estimate_main, 0.0f)),
      gap_(std::max(config.gap, 0.0f)),
      tree_(capacity_ + 1, 0.0f),
      size_(capacity_, estimate_),
      measured_flag_(capacity_, 0) {}

bool ItemGeometry::set_count(std::size_t count) noexcept {
    if (count > capacity_) {
        return false;
    }
    if (count < count_) {
        for (std::size_t index = count; index < count_; ++index) {
            if (measured_flag_[index] != 0) {
                measured_flag_[index] = 0;
                --measured_;
            }
        }
    } else if (count > count_) {
        for (std::size_t index = count_; index < count; ++index) {
            size_[index] = estimate_;
            measured_flag_[index] = 0;
        }
    }
    count_ = count;
    rebuild();
    return true;
}

bool ItemGeometry::insert(std::size_t index, std::size_t count) noexcept {
    if (count == 0) {
        return true;
    }
    if (index > count_ || count > capacity_ - count_) {
        return false;
    }
    std::move_backward(size_.begin() + index, size_.begin() + count_,
                       size_.begin() + count_ + count);
    std::move_backward(measured_flag_.begin() + index, measured_flag_.begin() + count_,
                       measured_flag_.begin() + count_ + count);
    std::fill(size_.begin() + index, size_.begin() + index + count, estimate_);
    std::fill(measured_flag_.begin() + index, measured_flag_.begin() + index + count,
              static_cast<std::uint8_t>(0));
    count_ += count;
    rebuild();
    return true;
}

bool ItemGeometry::remove(std::size_t index, std::size_t count) noexcept {
    if (count == 0) {
        return true;
    }
    if (index >= count_) {
        return false;
    }
    const std::size_t removed = std::min(count, count_ - index);
    for (std::size_t i = index; i < index + removed; ++i) {
        if (measured_flag_[i] != 0) {
            --measured_;
        }
    }
    std::move(size_.begin() + index + removed, size_.begin() + count_, size_.begin() + index);
    std::move(measured_flag_.begin() + index + removed, measured_flag_.begin() + count_,
              measured_flag_.begin() + index);
    count_ -= removed;
    rebuild();
    return true;
}

bool ItemGeometry::is_measured(std::size_t index) const noexcept {
    return index < count_ && measured_flag_[index] != 0;
}

float ItemGeometry::item_main_size(std::size_t index) const noexcept {
    return index < count_ ? size_[index] : 0.0f;
}

float ItemGeometry::item_offset(std::size_t index) const noexcept {
    const std::size_t clamped = std::min(index, count_);
    const float value = prefix(clamped);
    // The trailing per-item gap belongs between items, not after the last one.
    return clamped == count_ && count_ > 0 ? value - gap_ : value;
}

float ItemGeometry::total_extent() const noexcept {
    return count_ == 0 ? 0.0f : std::max(prefix(count_) - gap_, 0.0f);
}

std::size_t ItemGeometry::index_at_offset(float offset) const noexcept {
    if (count_ == 0 || offset <= 0.0f) {
        return 0;
    }
    if (offset >= total_extent()) {
        return count_ - 1;
    }
    const std::size_t next = upper_bound_prefix(offset);
    return next > 0 ? next - 1 : 0;
}

ItemGeometry::Range ItemGeometry::visible_range(float scroll_offset,
                                                float viewport_extent) const noexcept {
    const float total = total_extent();
    if (count_ == 0 || viewport_extent <= 0.0f || total <= 0.0f) {
        return Range{0, 0};
    }
    const float start = std::max(scroll_offset, 0.0f);
    if (start >= total) {
        return Range{count_, count_};
    }
    std::size_t first = 0;
    if (start > 0.0f) {
        // First item starting after the scroll offset is `next`; the visible one is before it.
        const std::size_t next = upper_bound_prefix(start);
        first = next > 0 ? next - 1 : 0;
    }
    const std::size_t last = std::min(lower_bound_prefix(start + viewport_extent), count_);
    return Range{first, std::max(first, last)};
}

ItemGeometry::MeasureChange ItemGeometry::refine_measure(std::size_t index,
                                                         float main_size) noexcept {
    MeasureChange change;
    if (index >= count_) {
        return change;
    }
    const float new_main = std::max(main_size, 0.0f);
    change.old_main = size_[index];
    change.new_main = new_main;
    change.delta = new_main - change.old_main;
    change.became_measured = measured_flag_[index] == 0;
    change.changed = change.delta != 0.0f || change.became_measured;
    if (change.changed) {
        if (change.became_measured) {
            measured_flag_[index] = 1;
            ++measured_;
        }
        size_[index] = new_main;
        if (change.delta != 0.0f) {
            tree_add(index, change.delta);
        }
    }
    change.suffix_offset = item_offset(index + 1);
    return change;
}

ItemGeometry::MeasureChange ItemGeometry::reset_measure(std::size_t index) noexcept {
    MeasureChange change;
    if (index >= count_) {
        return change;
    }
    change.old_main = size_[index];
    change.new_main = estimate_;
    if (measured_flag_[index] != 0) {
        change.became_estimate = true;
        change.delta = estimate_ - change.old_main;
        change.changed = true;
        measured_flag_[index] = 0;
        --measured_;
        size_[index] = estimate_;
        if (change.delta != 0.0f) {
            tree_add(index, change.delta);
        }
    }
    change.suffix_offset = item_offset(index + 1);
    return change;
}

void ItemGeometry::clear_measurements() noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
        measured_flag_[index] = 0;
        size_[index] = estimate_;
    }
    measured_ = 0;
    rebuild();
}

bool ItemGeometry::reseed(float estimate_main, float gap) noexcept {
    const float next_estimate = std::max(estimate_main, 0.0f);
    const float next_gap = std::max(gap, 0.0f);
    if (next_estimate == estimate_ && next_gap == gap_) {
        return false;
    }
    estimate_ = next_estimate;
    gap_ = next_gap;
    // Only the entries that still hold the estimate follow it; a measured size is a fact about the
    // item, not about the default.
    for (std::size_t index = 0; index < count_; ++index) {
        if (measured_flag_[index] == 0) {
            size_[index] = estimate_;
        }
    }
    rebuild();
    return true;
}

float ItemGeometry::prefix(std::size_t count) const noexcept {
    float sum = 0.0f;
    for (std::size_t index = count; index > 0; index -= lowbit(index)) {
        sum += tree_[index];
    }
    return sum;
}

std::size_t ItemGeometry::lower_bound_prefix(float target) const noexcept {
    // Smallest i in [0, count] with prefix(i) >= target.
    if (target <= 0.0f) {
        return 0;
    }
    std::size_t position = 0;
    float sum = 0.0f;
    for (std::size_t step = std::bit_floor(count_); step != 0; step >>= 1) {
        const std::size_t next = position + step;
        if (next <= count_ && sum + tree_[next] < target) {
            position = next;
            sum += tree_[next];
        }
    }
    return std::min(position + 1, count_);
}

std::size_t ItemGeometry::upper_bound_prefix(float target) const noexcept {
    // Smallest i in [0, count] with prefix(i) > target.
    if (target < 0.0f) {
        return 0;
    }
    std::size_t position = 0;
    float sum = 0.0f;
    for (std::size_t step = std::bit_floor(count_); step != 0; step >>= 1) {
        const std::size_t next = position + step;
        if (next <= count_ && sum + tree_[next] <= target) {
            position = next;
            sum += tree_[next];
        }
    }
    return std::min(position + 1, count_);
}

void ItemGeometry::tree_add(std::size_t index, float delta) noexcept {
    for (std::size_t i = index + 1; i <= capacity_; i += lowbit(i)) {
        tree_[i] += delta;
    }
}

void ItemGeometry::rebuild() noexcept {
    std::fill(tree_.begin(), tree_.end(), 0.0f);
    for (std::size_t index = 0; index < count_; ++index) {
        tree_add(index, size_[index] + gap_);
    }
}

} // namespace tessera::ui::virtualization
