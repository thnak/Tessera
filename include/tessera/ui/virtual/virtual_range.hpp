// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/item_geometry.hpp"
#include "tessera/ui/virtual/item_state_store.hpp"
#include "tessera/ui/virtual/key_index.hpp"
#include "tessera/ui/virtual/node_recycler.hpp"
#include "tessera/ui/virtual/scroll_anchor.hpp"
#include "tessera/ui/virtual/virtual_data_source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace tessera::ui {
class Node;
} // namespace tessera::ui

namespace tessera::ui::virtualization {

enum class VirtualRangeError : std::uint8_t {
    CapacityExceeded, // the data source has more items than the geometry capacity
    DuplicateKey,     // the data source repeated a stable key
    UnknownKey,       // scroll_to_key was given a key the last sync did not see
    IndexOutOfRange,  // apply_measure named an item the last sync did not see
};

// Placement target for `VirtualRange::scroll_to_index` (`docs/specs/virtualization-v1.md` section
// 7). The item is placed on the main axis; the cross axis never changes.
enum class ScrollAlignment : std::uint8_t {
    Start,   // item main start at the viewport start
    Center,  // item center at the viewport center
    End,     // item main end at the viewport end
    Nearest, // shortest move that makes the item fully visible; no move when it already is
};

// One step of the collection's sequential focus order (spec section 7: Tab/arrow navigation).
// Steps clamp at the collection ends and never wrap.
enum class FocusStep : std::uint8_t {
    Next,
    Previous,
    First,
    Last,
};

// One virtualized collection: data source + prefix-sum geometry + key index + node recycler
// (`docs/specs/virtualization-v1.md` sections 2-6).
//
// `update` is the scroll-time entry point. It computes the overscanned window from the prefix sums,
// releases realized nodes that left the window, acquires/builds the ones that entered it, measures
// each newly realized item, and re-anchors the scroll after every measurement that moved items
// before the viewport start (`reanchor_scroll`). Realized nodes are children of the caller's
// container node, positioned on the main axis in content space; the container's content size is
// kept equal to the measured total extent.
//
// Accounting: `realized_count() <= pool_cap`, every realized node is an in-use recycler slot, and
// when the window is wider than the pool the realized set truncates to the first `pool_cap` items
// (`VirtualRangeUpdate::truncated`). Once the pool is warm, `update` performs no heap allocation:
// the geometry, key index, realized table and free ring are all fixed-capacity. The data source may
// allocate inside `build`; that cost belongs to the component, not to the collection's bookkeeping.
//
// Determinism: no wall clock, RNG, or pointer-order decisions. Releases run in realized-table
// position order, builds run in ascending index order, and the recycler's FIFO ring decides which
// node is reused, so two identical scripts produce identical build/release sequences and offsets.
class VirtualRange {
public:
    struct Config {
        std::size_t capacity = 0; // item-count hard cap (geometry + key index)
        std::size_t pool_cap = kDefaultVirtualListPoolCap;
        std::size_t state_capacity = ItemStateStore::kDefaultCapacity; // keyed item state entries
        float estimate_main = kItemEstimateMain;
        float gap = 0.0f;
        float overscan_main = kDefaultVirtualListOverscanPx;
        float cross_extent = 0.0f; // passed to measure and used for bounds/content size
        bool horizontal = false;   // main axis is x instead of y
    };

    struct UpdateResult {
        ItemGeometry::Range window{};   // requested window including overscan
        ItemGeometry::Range realized{}; // indices actually realized (truncated by pool_cap)
        std::size_t built = 0;
        std::size_t released = 0;
        std::size_t measured = 0; // newly measured this update
        std::size_t reused = 0;   // builds served from the pool (built - new nodes)
        float scroll_offset = 0.0f;
        bool truncated = false;
        bool shifted = false; // a measurement before the anchor moved the scroll
        bool clamped = false; // the requested scroll fell outside [0, total - viewport]
    };

    struct ScrollTarget {
        std::size_t index = 0;
        float scroll_offset = 0.0f;
    };

    // Outcome of `focus_item`/`step_focus`: the resolved index, the offset the range settled on,
    // and whether a node currently shows the item. `realized == false` is not an error: a
    // zero-extent viewport or a pool-cap truncation focuses the key anyway, and the next `update`
    // that realizes the item restores the focus state on its node.
    struct FocusResult {
        std::size_t index = 0;
        VirtualItemId key{};
        float scroll_offset = 0.0f;
        bool realized = false;
        bool scrolled = false;  // the viewport offset changed
        bool clamped = false;   // the requested offset fell outside the scrollable range
        bool truncated = false; // the pool cap refused the target node
        std::size_t built = 0;
        std::size_t released = 0;
    };

    // `container` owns the realized item nodes; `source` must outlive the range.
    VirtualRange(Config config, VirtualDataSource& source, ui::Node& container);
    ~VirtualRange();

    VirtualRange(const VirtualRange&) = delete;
    VirtualRange& operator=(const VirtualRange&) = delete;

    // Snapshots item count and keys. Realized nodes are released because a data change moves
    // indices; call `update` next. On error nothing is realized and the previous count is kept.
    [[nodiscard]] std::expected<void, VirtualRangeError> sync();

    // Realizes the overscanned window at `scroll_offset` and returns the re-anchored offset.
    [[nodiscard]] std::expected<UpdateResult, VirtualRangeError> update(float scroll_offset,
                                                                        float viewport_extent);

    // Applies an asynchronous measurement for an already-known item: refines the prefix sums and
    // re-anchors the scroll if the item sits before the viewport anchor.
    [[nodiscard]] std::expected<AnchorAdjustment, VirtualRangeError>
    apply_measure(std::size_t index, float main_size);

    // Resolves a stable key and returns the scroll offset that puts it at `align` (0 = viewport
    // start, 1 = viewport end). The caller runs `update` with that offset to realize the target.
    [[nodiscard]] std::expected<ScrollTarget, VirtualRangeError>
    scroll_to_key(VirtualItemId key, float viewport_extent, float align = 0.0f);

    // Scroll offset that puts `index` at `alignment` in the viewport, clamped to
    // [0, total_extent - viewport_extent]. This is a pure query: it never calls `sync`, releases a
    // node, or rebuilds the geometry, so it cannot trigger a full resync (spec section 6). The
    // offsets come from the current prefix sums, i.e. estimates for unmeasured items; the caller
    // runs `update` with the offset, which measures the newly realized items and may re-anchor:
    // a measurement before the viewport anchor shifts the scroll by the same delta so the anchor
    // keeps its place, and `update`'s `scroll_offset` is then authoritative. `Nearest` keeps the
    // current scroll when the item is already fully visible, which makes it the arrow-key default.
    [[nodiscard]] std::expected<ScrollTarget, VirtualRangeError>
    scroll_to_index(std::size_t index, float viewport_extent, ScrollAlignment alignment);

    // Keyed focus and selection (spec section 7). Both live in the range's `ItemStateStore`, keyed
    // by `VirtualItemId`, never on a node: a recycled node carries neither, and both survive
    // scrolling, recycling, and data updates that keep the key. Nodes only mirror the state:
    // `update` and every focus call set `style::PseudoState::Focus` on the realized node of the
    // focused key and leave every other realized node without it, so at most one realized node is
    // focused, and releasing the focused item clears the bit with the node.
    [[nodiscard]] ItemStateStore& item_state() noexcept {
        return item_state_;
    }
    [[nodiscard]] const ItemStateStore& item_state() const noexcept {
        return item_state_;
    }

    // Focuses an existing key without scrolling. False for a key the last `sync` did not see, or
    // when the store refused a new pinned entry (every entry already pinned).
    bool set_focus(VirtualItemId key) noexcept;
    bool clear_focus() noexcept;
    [[nodiscard]] bool has_focus() const noexcept {
        return item_state_.has_focus();
    }
    [[nodiscard]] VirtualItemId focused_key() const noexcept {
        return item_state_.focused_key();
    }
    [[nodiscard]] bool is_focused(VirtualItemId key) const noexcept {
        return item_state_.is_focused(key);
    }
    // Realized node currently showing the focused item, or nullptr when there is no focus or the
    // item is scrolled out.
    [[nodiscard]] ui::Node* focused_node() const noexcept;

    // The section 7 "realize + scroll + focus" path: scrolls the key into view with `alignment`,
    // realizes it, then focuses it. Fails with UnknownKey for a key the last sync did not see.
    [[nodiscard]] std::expected<FocusResult, VirtualRangeError>
    focus_item(VirtualItemId key, float viewport_extent,
               ScrollAlignment alignment = ScrollAlignment::Nearest);

    // Tab/arrow stepping: moves focus one item from the focused item, realizes and scrolls it, and
    // leaves exactly one realized node focused. With no focus yet, `Next`/`Previous` both take the
    // first visible item instead of skipping it; `First`/`Last` jump to the ends. Steps clamp at
    // the collection ends rather than wrapping, so focus can never leave the collection.
    [[nodiscard]] std::expected<FocusResult, VirtualRangeError>
    step_focus(FocusStep step, float viewport_extent,
               ScrollAlignment alignment = ScrollAlignment::Nearest);

    // Selection by key, bounded by the item-state store capacity (see `ItemStateStore`). Unknown
    // keys are rejected. `selected_items` writes the selected keys in data order (ascending current
    // index), drops keys the last sync no longer resolves, and returns the number written; it is
    // allocation-free, so a header can query the selection on the hot path.
    bool set_selected(VirtualItemId key, bool selected) noexcept;
    bool toggle_selected(VirtualItemId key) noexcept;
    [[nodiscard]] bool is_selected(VirtualItemId key) const noexcept;
    [[nodiscard]] std::size_t selected_count() const noexcept {
        return item_state_.selected_count();
    }
    [[nodiscard]] std::size_t selected_items(std::span<VirtualItemId> out) const noexcept;
    bool clear_selection() noexcept;

    [[nodiscard]] std::optional<std::size_t> index_of(VirtualItemId key) const noexcept;
    [[nodiscard]] VirtualItemId key_at(std::size_t index) const noexcept;

    // Node realized for `index`, or nullptr when it is outside the window (or truncated away).
    [[nodiscard]] ui::Node* realized_node(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t realized_count() const noexcept {
        return realized_count_;
    }
    // Index at `position` in the realized table (build order, not sorted after scrolling back).
    [[nodiscard]] std::optional<std::size_t> realized_index_at(std::size_t position) const noexcept;

    [[nodiscard]] std::size_t item_count() const noexcept {
        return geometry_.item_count();
    }
    [[nodiscard]] const ItemGeometry& geometry() const noexcept {
        return geometry_;
    }
    [[nodiscard]] const NodeRecycler& recycler() const noexcept {
        return recycler_;
    }
    [[nodiscard]] const ScrollAnchor& anchor() const noexcept {
        return anchor_;
    }
    [[nodiscard]] bool has_anchor() const noexcept {
        return has_anchor_;
    }
    [[nodiscard]] float scroll_offset() const noexcept {
        return scroll_offset_;
    }
    [[nodiscard]] float viewport_extent() const noexcept {
        return viewport_extent_;
    }

private:
    struct RealizedItem {
        std::size_t index = 0;
        NodeRecycler::Lease lease{};
        float cross_size = 0.0f;
    };

    struct Refinement {
        ItemGeometry::MeasureChange change{};
        AnchorAdjustment adjustment{};
    };

    [[nodiscard]] ItemGeometry::Range expanded_window(float scroll_offset,
                                                      float viewport_extent) const noexcept;
    void set_anchor_from(std::size_t index) noexcept;
    [[nodiscard]] std::size_t resolve_anchor_index() const noexcept;
    [[nodiscard]] Refinement refine_and_reanchor(std::size_t index, float main_size) noexcept;
    void release_at(std::size_t position, bool notify_source) noexcept;
    void release_all(bool notify_source) noexcept;
    void place_realized_nodes() noexcept;
    void update_container_content_size() noexcept;
    // Mirrors the keyed focus onto the realized nodes: exactly the node whose key is focused gets
    // `PseudoState::Focus`, every other realized node loses it. Cheap and idempotent (`Node` only
    // dirties style when the bit actually changes), so `update` can call it unconditionally.
    void apply_focus_state() noexcept;
    // Focuses `index` (already resolved), realizes it, and copies the update counts into the
    // result.
    [[nodiscard]] FocusResult focus_resolved_index(std::size_t index, float viewport_extent,
                                                   ScrollAlignment alignment);
    // Index the next step starts from: the focused item, else the first visible item, else 0.
    [[nodiscard]] std::size_t focus_step_origin() const noexcept;

    Config config_;
    VirtualDataSource* source_ = nullptr;
    ui::Node* container_ = nullptr;
    ItemGeometry geometry_;
    KeyIndex keys_;
    NodeRecycler recycler_;
    ItemStateStore item_state_;
    ScrollAnchor anchor_{};
    bool has_anchor_ = false;
    float scroll_offset_ = 0.0f;
    float viewport_extent_ = 0.0f;

    // Realized slots are allocated once; every entry owns a detached lease whose node is a child
    // of `container_`.
    std::vector<RealizedItem> realized_;
    std::size_t realized_count_ = 0;
};

} // namespace tessera::ui::virtualization
