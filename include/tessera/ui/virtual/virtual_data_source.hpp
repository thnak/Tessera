// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/virtual_item.hpp"

#include <cstddef>

namespace tessera::ui {
class Node;
} // namespace tessera::ui

namespace tessera::ui::virtualization {

// Main-axis overscan default from `ui.virtualListOverscanPx`
// (`docs/specs/buffer-config-v1.md` section 2): extra realized extent on each side of the
// viewport.
inline constexpr float kDefaultVirtualListOverscanPx = 200.0f;

// Constraints handed to `VirtualDataSource::measure`. `cross_extent` is the available cross-axis
// extent in px (0 = unconstrained); a vertical list passes the viewport width. The constraints are
// part of the measurement cache key, so the same item may measure differently across widths.
struct MeasureConstraints {
    float cross_extent = 0.0f;

    [[nodiscard]] friend constexpr bool operator==(const MeasureConstraints&,
                                                   const MeasureConstraints&) noexcept = default;
};

// Read-only item source for one virtualized collection (`docs/specs/virtualization-v1.md`
// section 3).
//
// The source is the authority for item data and is never mutated by the collection: `item_count`,
// `item_key` and `measure` are const, and the collection only ever calls them. Render objects live
// in the component's keyed `ItemStateStore`, not in these nodes, so nothing the collection recycles
// can flow back into `build`.
//
// `build` fills a caller-owned node that has just been acquired from the recycler and cleared, so
// the node is clean on entry. It must write every field it depends on and must not read the
// node's previous contents; that is the "never sees recycled state" rule. `release` is an optional
// notification (for example to cancel prefetch) and must not retain the node.
class VirtualDataSource {
public:
    VirtualDataSource() = default;
    virtual ~VirtualDataSource() = default;

    VirtualDataSource(const VirtualDataSource&) = delete;
    VirtualDataSource& operator=(const VirtualDataSource&) = delete;

    [[nodiscard]] virtual std::size_t item_count() const = 0;
    [[nodiscard]] virtual VirtualItemId item_key(std::size_t index) const = 0;
    // Pure and deterministic for one `(index, constraints)` pair; may read a bounded measurement
    // cache but must not depend on scroll position, wall clock, or RNG.
    [[nodiscard]] virtual VirtualItemMeasure
    measure(std::size_t index, const MeasureConstraints& constraints) const = 0;
    // Populates `out` from the item model. `out` is clean and detached from the scene.
    virtual void build(std::size_t index, ui::Node& out) = 0;
    // Optional: called before the node is recycled. Default is a no-op; the recycler clears the
    // node itself, so a source that does not need the notification leaves this alone.
    virtual void release(std::size_t /*index*/, ui::Node& /*out*/) noexcept {}
};

} // namespace tessera::ui::virtualization
