// SPDX-License-Identifier: MIT
//
// Query/measure entry points of `VirtualRange`, split from `virtual_range.cpp` so both translation
// units stay inside the source-file soft limit.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <algorithm>

namespace tessera::ui::virtualization {

std::expected<AnchorAdjustment, VirtualRangeError> VirtualRange::apply_measure(std::size_t index,
                                                                               float main_size) {
    if (index >= geometry_.item_count()) {
        return std::unexpected(VirtualRangeError::IndexOutOfRange);
    }
    const Refinement refinement = refine_and_reanchor(index, main_size);
    if (refinement.change.changed) {
        // Positions and the reported content extent depend on the refined size right away; the
        // realized set itself does not change until the next update.
        place_realized_nodes();
        update_container_content_size();
    }
    return refinement.adjustment;
}

std::expected<VirtualRange::ScrollTarget, VirtualRangeError>
VirtualRange::scroll_to_key(VirtualItemId key, float viewport_extent, float align) {
    const std::optional<std::size_t> index = keys_.find(key);
    if (!index) {
        return std::unexpected(VirtualRangeError::UnknownKey);
    }
    const float viewport = std::max(viewport_extent, 0.0f);
    const float fraction = std::clamp(align, 0.0f, 1.0f);
    const float maximum = std::max(geometry_.total_extent() - viewport, 0.0f);
    const float offset =
        std::clamp(geometry_.item_offset(*index) - fraction * viewport, 0.0f, maximum);
    return ScrollTarget{*index, offset};
}

std::expected<VirtualRange::ScrollTarget, VirtualRangeError>
VirtualRange::scroll_to_index(std::size_t index, float viewport_extent, ScrollAlignment alignment) {
    if (index >= geometry_.item_count()) {
        return std::unexpected(VirtualRangeError::IndexOutOfRange);
    }
    const float viewport = std::max(viewport_extent, 0.0f);
    const float maximum = std::max(geometry_.total_extent() - viewport, 0.0f);
    const float start = geometry_.item_offset(index);
    const float extent = geometry_.item_main_size(index);
    const float end = start + extent;
    float offset = start;
    switch (alignment) {
    case ScrollAlignment::Start:
        offset = start;
        break;
    case ScrollAlignment::Center:
        offset = start - (viewport - extent) * 0.5f;
        break;
    case ScrollAlignment::End:
        offset = end - viewport;
        break;
    case ScrollAlignment::Nearest:
        if (start >= scroll_offset_ && end <= scroll_offset_ + viewport) {
            offset = scroll_offset_; // already fully visible: no scroll at all
        } else if (start < scroll_offset_ || end - start > viewport) {
            offset = start; // above the viewport, or taller than it: top-align
        } else {
            offset = end - viewport; // below the viewport: bottom-align
        }
        break;
    }
    // The clamp is the documented rule: the collection never scrolls past its measured end, so a
    // target near the tail settles one viewport short of its alignment instead of overscrolling.
    return ScrollTarget{index, std::clamp(offset, 0.0f, maximum)};
}

std::optional<std::size_t> VirtualRange::index_of(VirtualItemId key) const noexcept {
    return keys_.find(key);
}

VirtualItemId VirtualRange::key_at(std::size_t index) const noexcept {
    return keys_.key_at(index);
}

ui::Node* VirtualRange::realized_node(std::size_t index) const noexcept {
    for (std::size_t position = 0; position < realized_count_; ++position) {
        if (realized_[position].index == index) {
            return realized_[position].lease.node;
        }
    }
    return nullptr;
}

std::optional<std::size_t> VirtualRange::realized_index_at(std::size_t position) const noexcept {
    if (position >= realized_count_) {
        return std::nullopt;
    }
    return realized_[position].index;
}

} // namespace tessera::ui::virtualization
