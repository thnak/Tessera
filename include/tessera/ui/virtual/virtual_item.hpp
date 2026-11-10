// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui::virtualization {

// Stable per-item identity supplied by the read-only data source. Keys must survive inserts,
// removes and moves, so the collection never derives identity from an index
// (`docs/specs/virtualization-v1.md` section 3).
struct VirtualItemId {
    std::uint64_t value = 0;

    [[nodiscard]] friend constexpr bool operator==(const VirtualItemId& lhs,
                                                   const VirtualItemId& rhs) noexcept = default;
};

// Deterministic hash (splitmix64 finalizer) used by the fixed-capacity bucket tables. It has no
// random seed, so the same key maps to the same slot on every run and platform.
[[nodiscard]] constexpr std::uint64_t hash_virtual_item_id(VirtualItemId id) noexcept {
    std::uint64_t x = id.value + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Main/cross sizes returned by the data source's `measure` contract (spec section 3). The main
// axis is the scroll axis; the cross axis is width inside a row (or height inside a column for a
// horizontal list).
struct VirtualItemMeasure {
    float main_size = 0.0f;
    float cross_size = 0.0f;
};

// Default main-axis estimate for unmeasured items. 32 px matches the `estimateSize` value in the
// spec's section 10 IR example; the estimator strategy itself is deliberately one constant so
// prefix sums and visible ranges stay deterministic (section 4).
inline constexpr float kItemEstimateMain = 32.0f;

} // namespace tessera::ui::virtualization
