// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <unordered_map>
#include <vector>

namespace tessera::automation {

enum class VirtualCollectionError : std::uint8_t {
    IndexOutOfRange,  // the index is outside the collection's current item count
    CapacityExceeded, // the underlying range refused the window (geometry/pool cap)
};

// Outcome of revealing one index: where the collection settled and which items ended up realized.
struct VirtualScrollOutcome {
    std::size_t index = 0;
    float scroll_offset = 0.0f;
    std::size_t realized_first = 0; // first realized item index (inclusive)
    std::size_t realized_last = 0;  // one past the last realized item index
    bool clamped = false;
};

// One live virtual collection exposed to the automation host
// (`docs/specs/headless-automation-v1.md` section 6.8). A script cannot address an unrealized item
// by node (there is none), so `scrollIntoView` with an `itemIndex` resolves the collection node,
// asks this seam to realize the item, and the realized node then carries the collection's `uiId`
// plus the item's key and is addressable like any other node.
//
// Implementations own the geometry/realization (`VirtualRange` for lists, `VirtualGridRows` for
// grids) and the item nodes, whose scene parents are the collection container. All calls are
// synchronous on the UI thread.
class VirtualCollection {
public:
    VirtualCollection() = default;
    virtual ~VirtualCollection() = default;

    VirtualCollection(const VirtualCollection&) = delete;
    VirtualCollection& operator=(const VirtualCollection&) = delete;

    // The collection container node the scene owns; realized items are its children.
    [[nodiscard]] virtual ui::Node& container() noexcept = 0;
    [[nodiscard]] virtual std::size_t item_count() const noexcept = 0;

    // Current viewport offset along the scroll axis, in content pixels.
    [[nodiscard]] virtual float scroll_offset() const noexcept = 0;

    // Calls the collection's scroll-to-index path, realizes the resulting window, and places the
    // realized nodes. The viewport extent comes from the container's laid-out bounds.
    [[nodiscard]] virtual std::expected<VirtualScrollOutcome, VirtualCollectionError>
    scroll_to_index(std::size_t index, ui::virtualization::ScrollAlignment alignment) = 0;

    // Re-applies the realized window after a layout pass (which rewrites item bounds from flow).
    // A no-op window never builds or releases; it only re-places the existing nodes.
    [[nodiscard]] virtual std::expected<void, VirtualCollectionError> refresh() = 0;

    // Node currently realizing `index`, or nullptr when it is outside the window.
    [[nodiscard]] virtual ui::Node* realized_node(std::size_t index) const noexcept = 0;
};

// Node -> collection lookup for one scene. The harness scene registers every virtual collection it
// builds; the automation host resolves a `scrollIntoView` target through it. Pointers are borrowed;
// the collections must outlive the registry.
class VirtualCollectionRegistry {
public:
    void add(ui::Node& container, VirtualCollection& collection);
    [[nodiscard]] VirtualCollection* find(const ui::Node& container) const noexcept;
    [[nodiscard]] bool empty() const noexcept {
        return collections_.empty();
    }

    // Re-places every collection's realized window after a layout pass. The automation host calls
    // this after settling so a snapshot never reports flow bounds for windowed item nodes.
    void refresh_all();

private:
    std::unordered_map<const ui::Node*, VirtualCollection*> by_node_;
    std::vector<VirtualCollection*> collections_;
};

} // namespace tessera::automation
