// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/scroll_anchor.hpp"

#include <algorithm>

namespace tessera::ui::virtualization {

AnchorAdjustment reanchor_scroll(float scroll_offset, float viewport_extent, float total_extent,
                                 std::size_t anchor_index, std::size_t refined_index,
                                 float delta) noexcept {
    AnchorAdjustment adjustment;
    float candidate = scroll_offset;
    if (delta != 0.0f && refined_index < anchor_index) {
        candidate = scroll_offset + delta;
        adjustment.shifted = true;
    }
    const float max_scroll = std::max(total_extent - std::max(viewport_extent, 0.0f), 0.0f);
    const float clamped = std::clamp(candidate, 0.0f, max_scroll);
    adjustment.clamped = clamped != candidate;
    adjustment.scroll_offset = clamped;
    return adjustment;
}

} // namespace tessera::ui::virtualization
