// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/item_geometry.hpp"
#include "tessera/ui/virtual/node_recycler.hpp"
#include "tessera/ui/virtual/virtual_data_source.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace tessera::ui {
class Node;
} // namespace tessera::ui

namespace tessera::ui::virtualization {

// The realized window of one virtualized collection: which item nodes currently exist, where they
// sit, and the container's content extent (`docs/architecture/god-type-audit-v1.md` section 3.3).
//
// `VirtualRange` used to carry this table inline along with its geometry, key index, focus and
// selection policy, and re-export every one of those collaborators, so every caller depended on the
// aggregate. The window is the one part that owns real mutable state plus real placement logic
// (release, acquire-and-attach, main-axis placement, content size), so it becomes a type here and
// `VirtualRange` keeps only the scheduling that drives it.
//
// Accounting stays with the window because it is what knows the table: `count() <= capacity()`,
// every realized node is an in-use recycler lease, and the table is a packed array whose order is
// build order (`release_at` swaps the tail in, exactly as the previous inline code did, so the
// release/reuse sequences stay deterministic).
class RealizedWindow {
public:
    struct Item {
        std::size_t index = 0;
        NodeRecycler::Lease lease{};
        float cross_size = 0.0f;
    };

    explicit RealizedWindow(std::size_t pool_capacity)
        : items_(pool_capacity) {}

    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return items_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return count_ == 0;
    }

    // The entry at `position` in build order. Requires `position < count()`.
    [[nodiscard]] const Item& at(std::size_t position) const noexcept {
        return items_[position];
    }
    [[nodiscard]] Item& at(std::size_t position) noexcept {
        return items_[position];
    }

    // Node realized for `index`, or nullptr when it is outside the window (or truncated away).
    [[nodiscard]] ui::Node* node_for(std::size_t index) const noexcept;
    // Data index at `position` in the table (build order, not sorted after scrolling back).
    [[nodiscard]] std::optional<std::size_t> index_at(std::size_t position) const noexcept;
    [[nodiscard]] bool contains(std::size_t index) const noexcept {
        return node_for(index) != nullptr;
    }

    // Appends a realized entry. The caller has already attached the node to the container.
    void append(std::size_t index, NodeRecycler::Lease lease, float cross_size) noexcept {
        items_[count_] = Item{index, lease, cross_size};
        ++count_;
    }

    // Removes the entry at `position` and writes it to `out` so the caller can release the node
    // through its own recycler and source. The tail entry is moved into the hole; the table stays
    // packed.
    void take_at(std::size_t position, Item& out) noexcept;
    // Removes and returns the last entry.
    void take_last(Item& out) noexcept {
        take_at(count_ - 1, out);
    }

    void clear() noexcept {
        count_ = 0;
    }

    // Sets each realized node's bounds on the main axis at `scroll_offset`. Positions come from the
    // caller's geometry, so the window owns no measurement state of its own.
    void place(const ItemGeometry& geometry, float scroll_offset, bool horizontal,
               const Item& item) noexcept;

    // Positions every realized entry.
    void place_all(const ItemGeometry& geometry, float scroll_offset, bool horizontal) noexcept {
        for (std::size_t position = 0; position < count_; ++position) {
            place(geometry, scroll_offset, horizontal, items_[position]);
        }
    }

    // The realized window's inclusive index span, or `{0, 0}` when nothing is realized. This is the
    // `UpdateResult::realized` reporting shape: the caller gets one range instead of the table.
    [[nodiscard]] ItemGeometry::Range index_span() const noexcept;

private:
    std::vector<Item> items_;
    std::size_t count_ = 0;
};

} // namespace tessera::ui::virtualization
