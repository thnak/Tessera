// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/virtual_item.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::ui::virtualization {

// Main-axis geometry for one virtualized collection (a list, or grid rows).
//
// Items are addressed by index; identity and keys stay with the read-only data source. Sizes live
// in a Fenwick prefix-sum tree:
//   offset(i) = sum(size(j) + gap for j < i), total_extent = offset(count) - gap.
// Unmeasured items contribute `estimate_main` (32 px by default, spec section 10 `estimateSize`).
// A real measure replaces the estimate and shifts only the suffix after that index, so already
// measured items keep both their size and their offset; the collection never relayouts them.
//
// Bounded/capacity rule: `Config::capacity` is a hard cap allocated once in the constructor; every
// query and refinement below is allocation-free. `set_count`/`insert`/`remove` that would exceed
// the cap return false and leave the tables unchanged. Scroll-time calls (`item_offset`,
// `total_extent`, `visible_range`, `index_at_offset`, `refine_measure`) are O(log n);
// `insert`/`remove` are O(n) because the shifted suffix is rewritten. There is no wall clock, RNG,
// or data-source access in this class.
class ItemGeometry {
public:
    struct Config {
        std::size_t capacity = 0; // hard cap on item count
        float estimate_main = kItemEstimateMain;
        float gap = 0.0f; // main-axis gap between neighbouring items
    };

    // Half-open [first, last) index window.
    struct Range {
        std::size_t first = 0;
        std::size_t last = 0;

        [[nodiscard]] constexpr bool empty() const noexcept {
            return first >= last;
        }
        [[nodiscard]] constexpr std::size_t count() const noexcept {
            return last > first ? last - first : 0;
        }
    };

    // Result of refining or resetting one measurement. `delta` is added to the offset of every item
    // after `index` (the suffix shift); `suffix_offset` is the new offset of item `index + 1`.
    struct MeasureChange {
        bool changed = false;         // size or measured state changed
        bool became_measured = false; // estimate -> measured
        bool became_estimate = false; // measured -> estimate (reset)
        float old_main = 0.0f;
        float new_main = 0.0f;
        float delta = 0.0f;
        float suffix_offset = 0.0f;
    };

    explicit ItemGeometry(Config config);

    // Returns false (and changes nothing) when `count` exceeds capacity.
    [[nodiscard]] bool set_count(std::size_t count) noexcept;

    // Data-source insert/remove. New items start at the estimate; removed measurements are dropped.
    // Returns false when the index/range is outside [0, count] or the insert exceeds capacity.
    [[nodiscard]] bool insert(std::size_t index, std::size_t count) noexcept;
    [[nodiscard]] bool remove(std::size_t index, std::size_t count) noexcept;

    [[nodiscard]] std::size_t item_count() const noexcept {
        return count_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] std::size_t measured_count() const noexcept {
        return measured_;
    }
    [[nodiscard]] float estimate_main() const noexcept {
        return estimate_;
    }
    [[nodiscard]] float gap() const noexcept {
        return gap_;
    }

    [[nodiscard]] bool is_measured(std::size_t index) const noexcept;
    // Returns 0 for an out-of-range index.
    [[nodiscard]] float item_main_size(std::size_t index) const noexcept;
    // Clamped to [0, count]; `item_offset(count) == total_extent()`.
    [[nodiscard]] float item_offset(std::size_t index) const noexcept;
    [[nodiscard]] float total_extent() const noexcept;
    // Item containing `offset`, clamped to the first/last item. Returns 0 for an empty geometry.
    [[nodiscard]] std::size_t index_at_offset(float offset) const noexcept;
    // Items overlapping [scroll_offset, scroll_offset + viewport_extent). Empty for an empty
    // collection, a non-positive viewport, or a scroll offset at/after the total extent.
    [[nodiscard]] Range visible_range(float scroll_offset, float viewport_extent) const noexcept;

    // Measure-refinement entry points. `refine_measure` replaces an estimate (or corrects an older
    // measurement); `reset_measure` restores the estimate, e.g. when a cache evicts an offscreen
    // item. Both are no-ops for an out-of-range index.
    [[nodiscard]] MeasureChange refine_measure(std::size_t index, float main_size) noexcept;
    [[nodiscard]] MeasureChange reset_measure(std::size_t index) noexcept;
    void clear_measurements() noexcept;

private:
    [[nodiscard]] float prefix(std::size_t count) const noexcept;
    [[nodiscard]] std::size_t lower_bound_prefix(float target) const noexcept;
    [[nodiscard]] std::size_t upper_bound_prefix(float target) const noexcept;
    void tree_add(std::size_t index, float delta) noexcept;
    void rebuild() noexcept;

    std::size_t capacity_ = 0;
    std::size_t count_ = 0;
    std::size_t measured_ = 0;
    float estimate_ = kItemEstimateMain;
    float gap_ = 0.0f;
    std::vector<float> tree_; // Fenwick tree over (size + gap), 1-based storage
    std::vector<float> size_; // per-item main size (the estimate while unmeasured)
    std::vector<std::uint8_t> measured_flag_;
};

} // namespace tessera::ui::virtualization
