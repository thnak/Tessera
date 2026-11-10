// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/automation/id.hpp"
#include "tessera/ui/virtual/virtual_item.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tessera::ui::virtualization {

// `pos_in_set` is 1-based like ARIA's `aria-posinset`, so it is the item's 0-based data index plus
// this base. `set_size` is the collection's current item count (ARIA `aria-setsize`).
inline constexpr std::size_t kPosInSetBase = 1;

// Bound on the coalescing announcement ring (`docs/specs/virtualization-v1.md` section 7). One
// undrained announcement per kind is normally pending; the ring is larger so a future kind cannot
// silently grow the object, and a full ring drops its oldest entry instead of allocating.
inline constexpr std::size_t kMaxPendingAnnouncements = 4;

// Automation identity of one realized collection item (`docs/specs/automation-ids-v1.md` section
// 1): the item template's build-time `UiId` paired with the data-lifetime `ItemKey`. The pair, not
// a per-instance id, identifies a list/grid item; the collection reads the id off the realized node
// and never mints, re-numbers, or re-hashes one.
struct CollectionItemIdentity {
    automation::UiId ui_id = 0; // realized node's `automation_id`; 0 when not assigned
    VirtualItemId item_key{};

    [[nodiscard]] friend constexpr bool
    operator==(const CollectionItemIdentity&, const CollectionItemIdentity&) noexcept = default;
};

// Semantics view of one realized item (spec section 7). `index` is the 0-based data index used by
// the collection internally; `pos_in_set` is the 1-based position exposed to accessibility and
// automation consumers. Items outside the realized window have no view: only realized nodes exist
// in the scene graph, but `pos_in_set` for a key is always available from the key index.
struct CollectionItemView {
    CollectionItemIdentity identity{};
    std::size_t index = 0;
    std::size_t pos_in_set = kPosInSetBase;
    std::size_t set_size = 0;
    bool focused = false;
    bool selected = false;

    [[nodiscard]] friend constexpr bool operator==(const CollectionItemView&,
                                                   const CollectionItemView&) noexcept = default;
};

// One selected item in data order. `identity.ui_id` is 0 when the selected key is not realized (a
// selection may include unrealized items, spec section 7); the position still resolves through the
// key index.
struct SelectionEntry {
    CollectionItemIdentity identity{};
    std::size_t index = 0;
    std::size_t pos_in_set = kPosInSetBase;

    [[nodiscard]] friend constexpr bool operator==(const SelectionEntry&,
                                                   const SelectionEntry&) noexcept = default;
};

enum class AnnouncementKind : std::uint8_t {
    Focus,     // the focused key changed
    Selection, // the selection membership changed
};

// One bounded, structured announcement (`docs/specs/accessibility-v1.md` sections 6 and 8). It
// carries the position instead of a formatted sentence: platform bridges localize the fields, and
// `format_announcement` builds developer/debug text into a caller buffer without allocating.
struct Announcement {
    AnnouncementKind kind = AnnouncementKind::Focus;
    CollectionItemIdentity identity{};
    std::size_t index = 0;
    std::size_t pos_in_set = kPosInSetBase;
    std::size_t set_size = 0;
    bool selected = false;      // Selection only: the new membership state
    bool realized = false;      // whether a node showed the item when the change was observed
    std::uint64_t sequence = 0; // staged-announcement counter, strictly increasing

    [[nodiscard]] friend constexpr bool operator==(const Announcement&,
                                                   const Announcement&) noexcept = default;
};

struct AnnouncementStats {
    std::size_t staged = 0;            // announcements created (before coalescing)
    std::size_t superseded = 0;        // undrained announcement replaced by a newer one of its kind
    std::size_t dropped = 0;           // pending announcement dropped because the ring was full
    std::size_t taken = 0;             // announcements returned by `take_announcement`
    std::size_t focus_changes = 0;     // observed focus-key changes, including clears
    std::size_t selection_changes = 0; // observed selection revision bumps
};

// Collection semantics for one virtualized collection (spec section 7): set size, per-item
// `pos_in_set`, the realized focused item as the active descendant, the `(UiId, ItemKey)` identity
// of a realized item, the selection in data order, and coalesced focus/selection announcements.
//
// Everything is derived from the range: the key index answers positions, the realized table answers
// which items currently exist, and the item-state store answers focus/selection. Nothing is cached
// that a `sync`, `update`, or recycle could invalidate, so the object is always consistent with the
// range at call time.
//
// Announcements (`docs/specs/virtualization-v1.md` section 7 and accessibility section 6): the host
// calls `observe()` once after input/focus handling and before announcing, then drains with
// `take_announcement()`. `observe()` is edge-triggered on the focused key and on the store's
// selection revision, so it never re-announces on a frame where nothing changed. A burst of changes
// between two observations coalesces: an undrained announcement of the same kind is superseded in
// place (counted in `stats().superseded`), and the ring keeps at most one entry per kind, so the
// announce path is bounded and allocation-free. An unrealized-item focus still announces once with
// its position (`realized == false`); realizing the item later does not announce again.
class CollectionSemantics {
public:
    // `range` must outlive the semantics object. The two scratch buffers are sized from the range's
    // pool cap and item-state capacity, so construct after the range is configured; every hot-path
    // query below then allocates nothing.
    explicit CollectionSemantics(VirtualRange& range);

    CollectionSemantics(const CollectionSemantics&) = delete;
    CollectionSemantics& operator=(const CollectionSemantics&) = delete;

    // Set semantics: the current item count (ARIA `set_size`). `pos_in_set` is nullopt for an index
    // the last sync did not see, or for a key outside the data source.
    [[nodiscard]] std::size_t set_size() const noexcept {
        return range_.item_count();
    }
    [[nodiscard]] std::optional<std::size_t> pos_in_set(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> pos_in_set(VirtualItemId key) const noexcept;

    // Realized exposure: writes the realized items in data order (ascending current index) and
    // returns the number written. When `out` is smaller than the realized set, the prefix in data
    // order is exposed, so a bounded consumer never gets an arbitrary subset.
    [[nodiscard]] std::size_t realized_items(std::span<CollectionItemView> out) const noexcept;

    // Automation identity of a realized key. `ui_id_of` returns 0 for an unrealized key or a node
    // without an assigned id; `identity_of` is nullopt unless the key is currently realized.
    [[nodiscard]] automation::UiId ui_id_of(VirtualItemId key) const noexcept;
    [[nodiscard]] std::optional<CollectionItemIdentity>
    identity_of(VirtualItemId key) const noexcept;

    // Active descendant: the realized item currently carrying the focused key. Nullopt when there
    // is no focus, or when the focused key is not realized, because an active descendant must name
    // a node that exists. `focused_pos_in_set` still resolves the position from the key index, so
    // an unrealized focused item can be announced before it is realized.
    [[nodiscard]] std::optional<CollectionItemIdentity> active_descendant() const noexcept;
    [[nodiscard]] std::optional<std::size_t> focused_pos_in_set() const noexcept;

    // Selection exposure in data order, with each key's current position. Selected keys that the
    // last sync no longer resolves are dropped; the short-buffer rule matches `realized_items`.
    [[nodiscard]] std::size_t selection(std::span<SelectionEntry> out) const noexcept;

    // Stages an announcement when the focused key or the selection membership changed since the
    // previous call. Does not allocate and does not itself announce: the caller drains.
    void observe() noexcept;
    // Pops the oldest pending announcement (FIFO across kinds), or nullopt.
    [[nodiscard]] std::optional<Announcement> take_announcement() noexcept;
    [[nodiscard]] std::size_t pending_announcements() const noexcept {
        return pending_count_;
    }
    [[nodiscard]] const AnnouncementStats& stats() const noexcept {
        return stats_;
    }

    // Decimal `ItemKey` text for automation snapshots (`UiSnapshotNode::item_key` is a string).
    // Writes into `out`; returns an empty view when the buffer cannot hold a 64-bit decimal value
    // (20 characters).
    [[nodiscard]] static std::string_view format_item_key(VirtualItemId key,
                                                          std::span<char> out) noexcept;
    // Deterministic developer/debug text for one announcement. Platform bridges localize from the
    // structured fields instead (accessibility spec section 8); this helper exists for traces,
    // tests, and headless tooling, and it never allocates.
    [[nodiscard]] static std::string_view format_announcement(const Announcement& announcement,
                                                              std::span<char> out) noexcept;

private:
    void stage(AnnouncementKind kind, VirtualItemId key, bool selected) noexcept;

    VirtualRange& range_;
    // Scratch storage for the two data-order queries; `mutable` because they are pure caches
    // written by const accessors. Allocated once at construction, so the queries never allocate.
    mutable std::vector<CollectionItemView> realized_scratch_;
    mutable std::vector<SelectionEntry> selection_scratch_;

    std::array<Announcement, kMaxPendingAnnouncements> pending_{};
    std::size_t pending_head_ = 0;
    std::size_t pending_count_ = 0;
    std::uint64_t next_sequence_ = 1;

    bool has_observed_focus_ = false;
    VirtualItemId observed_focus_{};
    std::uint64_t observed_selection_revision_ = 0;

    AnnouncementStats stats_{};
};

} // namespace tessera::ui::virtualization
