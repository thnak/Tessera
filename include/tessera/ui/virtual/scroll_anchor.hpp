// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/virtual_item.hpp"

#include <cstddef>

namespace tessera::ui::virtualization {

// Viewport anchor from `docs/specs/virtualization-v1.md` section 6: the keyed item at the viewport
// start plus how far into that item the viewport begins. The owner resolves `key` to an index
// through the read-only data source; this module never reads the data source.
struct ScrollAnchor {
    VirtualItemId key{};
    float offset_in_item = 0.0f;
};

struct AnchorAdjustment {
    float scroll_offset = 0.0f;
    bool shifted = false; // the scroll moved because an item before the anchor changed size
    bool clamped = false; // the raw offset fell outside [0, total_extent - viewport_extent]
};

// Applies the section 6 re-anchor rule after the item at `refined_index` changed its main size by
// `delta`:
//   - refined item before the anchor: shift the scroll by `delta`, so the anchor keeps its
//     viewport-relative position while only the suffix after the measured item moves;
//   - refined item is the anchor: keep the scroll and clamp it (the anchor itself moved);
//   - refined item after the anchor: no adjustment.
// The result is always clamped to the scrollable range; `clamped` reports that.
[[nodiscard]] AnchorAdjustment reanchor_scroll(float scroll_offset, float viewport_extent,
                                               float total_extent, std::size_t anchor_index,
                                               std::size_t refined_index, float delta) noexcept;

} // namespace tessera::ui::virtualization
