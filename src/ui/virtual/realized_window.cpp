// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/realized_window.hpp"

#include <algorithm>
#include <limits>

namespace tessera::ui::virtualization {

ui::Node* RealizedWindow::node_for(std::size_t index) const noexcept {
    for (std::size_t position = 0; position < count_; ++position) {
        if (items_[position].index == index) {
            return items_[position].lease.node;
        }
    }
    return nullptr;
}

std::optional<std::size_t> RealizedWindow::index_at(std::size_t position) const noexcept {
    if (position >= count_) {
        return std::nullopt;
    }
    return items_[position].index;
}

void RealizedWindow::take_at(std::size_t position, Item& out) noexcept {
    out = items_[position];
    // Swap the tail into the hole rather than shifting: the table is a set, its order is build
    // order, and the previous inline code did the same, so release/reuse sequences stay
    // deterministic across two identical scripts.
    items_[position] = items_[count_ - 1];
    items_[count_ - 1] = Item{};
    --count_;
}

void RealizedWindow::place(const ItemGeometry& geometry, float scroll_offset, bool horizontal,
                           const Item& item) noexcept {
    const float offset = geometry.item_offset(item.index) - scroll_offset;
    const float main_size = geometry.item_main_size(item.index);
    if (horizontal) {
        item.lease.node->set_bounds(
            tessera::ui::rhi::Rect{offset, 0.0f, main_size, item.cross_size});
    } else {
        item.lease.node->set_bounds(
            tessera::ui::rhi::Rect{0.0f, offset, item.cross_size, main_size});
    }
}

ItemGeometry::Range RealizedWindow::index_span() const noexcept {
    if (count_ == 0) {
        return ItemGeometry::Range{0, 0};
    }
    std::size_t first = std::numeric_limits<std::size_t>::max();
    std::size_t last = 0;
    for (std::size_t position = 0; position < count_; ++position) {
        const std::size_t index = items_[position].index;
        first = std::min(first, index);
        last = std::max(last, index);
    }
    return ItemGeometry::Range{first, last + 1};
}

} // namespace tessera::ui::virtualization
