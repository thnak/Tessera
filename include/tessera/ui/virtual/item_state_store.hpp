// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/virtual_item.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace tessera::ui::virtualization {

// Per-item state owned by the collection (`docs/specs/virtualization-v1.md` sections 2 and 3).
//
// Nodes are recycled and hold no item state: everything that must survive scrolling lives here,
// keyed by the stable `VirtualItemId` from the read-only data source. The store never interprets
// the payload; the component maps its own state (selection, draft text, async status, ...) onto
// `payload`/`revision` and keeps the last measured size in `measured_main`.
//
// `focused` and `selected` are store-owned mirrors of the keyed focus/selection API below (spec
// section 7). They are not payload: `put` preserves them, and only `set_focus`/`set_selected`
// change them, so a component can copy a whole `ItemState` out, edit it, and put it back without
// accidentally moving focus or selection.
struct ItemState {
    std::uint64_t revision = 0; // data revision this state was produced from
    std::uint64_t payload = 0;  // component-defined bits or handle
    float measured_main = 0.0f; // last measured main-axis size; 0 = not measured
    float measured_cross = 0.0f;
    bool focused = false;  // this key is the store's focused key
    bool selected = false; // this key is in the store's selection set
};

// Bounded keyed store with a deterministic LRU eviction rule.
//
// Storage is allocated once in the constructor: a fixed-capacity dense entry array plus an
// open-addressed bucket index at <= 50% load, so `find`, `peek`, `put` and `erase` never allocate.
// Recency is a monotonic counter bumped on every successful `find`/`put` (no wall clock, no RNG).
// When an insert would exceed capacity, the entry with the smallest stamp that is neither focused
// nor selected is evicted; overwriting an existing key never evicts and never moves focus.
// Evictions are counted in `stats()`, so a component can refetch data for an evicted key. The
// documented capacity/eviction rule: capacity is a constructor argument (default 256) and eviction
// is always LRU over unpinned entries, never growth.
//
// Pinning (`docs/specs/virtualization-v1.md` section 7): the focused key and every selected key are
// never evicted, because losing them would silently move the user's focus or selection. Capacity
// therefore bounds the selection size: `set_selected` refuses (returns false) once every entry is
// pinned or selected, and so does an insert when no unpinned entry is left. A refusal changes
// nothing and is counted in `stats().refusals`. In the degenerate capacity-1 case a focused entry
// makes every other insert refuse; size the store for `selection + focus + payload` entries.
//
// Pointers returned by `find` stay valid until the next insert of a new key (which may evict),
// `erase`, `clear`, or destruction; copy the value out when it must outlive the call.
class ItemStateStore {
public:
    struct Stats {
        std::size_t entries = 0;
        std::size_t peak_entries = 0;
        std::size_t hits = 0;      // find/peek lookups that matched
        std::size_t misses = 0;    // find/peek lookups that did not match
        std::size_t inserts = 0;   // new keys stored (overwrites are not inserts)
        std::size_t evictions = 0; // entries dropped by the LRU rule
        std::size_t refusals = 0;  // inserts/selection rejected because nothing was evictable
    };

    static constexpr std::size_t kMinCapacity = 1;
    static constexpr std::size_t kDefaultCapacity = 256;

    explicit ItemStateStore(std::size_t capacity = kDefaultCapacity);

    // Lookups. `find` refreshes recency; `peek`/`contains` do not (use them for speculative reads
    // that must not disturb the eviction order). `find` and `peek` record a hit/miss in `stats()`;
    // `contains` is a plain probe and does not.
    [[nodiscard]] ItemState* find(VirtualItemId key) noexcept;
    [[nodiscard]] const ItemState* peek(VirtualItemId key) const noexcept;
    [[nodiscard]] bool contains(VirtualItemId key) const noexcept;

    // Inserts or overwrites payload/measure state. Returns true when the key was newly inserted and
    // false on overwrite or on a refusal (nothing evictable); see `stats().refusals`.
    bool put(VirtualItemId key, const ItemState& state) noexcept;

    // Focus/selection by key (`docs/specs/virtualization-v1.md` section 7). A recycled node never
    // carries either: the store is the single source of truth, so both survive scrolling, node
    // recycling, and data updates that keep the key.
    //
    // `set_focus` inserts a stub entry when needed, clears the previous key's `focused` mirror, and
    // pins the new entry. Returns false only when the entry cannot be created because every entry
    // is already pinned; the focus is unchanged in that case.
    bool set_focus(VirtualItemId key) noexcept;
    bool clear_focus() noexcept;
    [[nodiscard]] bool has_focus() const noexcept {
        return has_focus_;
    }
    [[nodiscard]] VirtualItemId focused_key() const noexcept {
        return focused_key_;
    }
    [[nodiscard]] bool is_focused(VirtualItemId key) const noexcept {
        return has_focus_ && key == focused_key_;
    }

    // `set_selected` is idempotent and keeps the selection set bounded by capacity: a new selected
    // key is refused (false) when no unpinned entry can be evicted. `toggle_selected` returns the
    // resulting membership (false when the insert was refused). `clear_selection` reports whether
    // anything was selected.
    bool set_selected(VirtualItemId key, bool selected) noexcept;
    bool toggle_selected(VirtualItemId key) noexcept;
    [[nodiscard]] bool is_selected(VirtualItemId key) const noexcept;
    [[nodiscard]] std::size_t selected_count() const noexcept {
        return selected_count_;
    }
    bool clear_selection() noexcept;

    // Selection change journal for semantics/announcement consumers
    // (`docs/specs/virtualization-v1.md` section 7). `selection_revision` moves forward on every
    // membership change; `last_selection_change` names the most recent one and is `valid == false`
    // after a clear, so a consumer can react to "something changed" without copying the set.
    struct SelectionChange {
        VirtualItemId key{};
        bool selected = false;
        bool valid = false;
    };

    [[nodiscard]] std::uint64_t selection_revision() const noexcept {
        return selection_revision_;
    }
    [[nodiscard]] SelectionChange last_selection_change() const noexcept {
        return last_selection_change_;
    }
    // Visits every selected key in entry order without allocating; the visitor may not mutate the
    // store. Used to project the selection into data order.
    template<typename Visitor> void for_each_selected(Visitor&& visitor) const noexcept {
        for (std::size_t slot = 0; slot < size_; ++slot) {
            if (entries_[slot].state.selected) {
                visitor(entries_[slot].key);
            }
        }
    }

    // Copies up to `out.size()` selected keys in entry order and returns the number written. The
    // caller maps keys onto data indices; `VirtualRange::selected_items` does that and returns data
    // order. Allocation-free so the selection can be queried on the hot path.
    [[nodiscard]] std::size_t copy_selected(std::span<VirtualItemId> out) const noexcept;

    bool erase(VirtualItemId key) noexcept;
    void clear() noexcept;

    // Erases every entry whose key `keep(key)` returns false, allocation-free. `VirtualRange::sync`
    // uses it to drop state (including focus/selection) for keys the data source no longer has.
    template<typename Keep> void retain_if(Keep&& keep) noexcept {
        for (std::size_t slot = size_; slot-- > 0;) {
            if (!keep(entries_[slot].key)) {
                erase_slot(slot);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] Stats stats() const noexcept {
        return stats_;
    }

private:
    struct Entry {
        VirtualItemId key{};
        ItemState state{};
        std::uint64_t stamp = 0;
    };

    static constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t kEmptyBucket = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] std::size_t bucket_index(VirtualItemId key) const noexcept;
    [[nodiscard]] std::size_t find_bucket(VirtualItemId key) const noexcept;
    [[nodiscard]] std::size_t find_slot(VirtualItemId key) const noexcept;
    void bucket_insert(std::size_t bucket, std::size_t slot) noexcept;
    void bucket_remove(std::size_t bucket) noexcept;
    void erase_slot(std::size_t slot) noexcept;
    void note_selection_change(VirtualItemId key, bool selected) noexcept {
        ++selection_revision_;
        last_selection_change_ = SelectionChange{key, selected, true};
    }
    // Slot for the least recently used entry that may be evicted, or kNoSlot when all are pinned.
    [[nodiscard]] std::size_t lru_victim() const noexcept;
    // Stores `key` in a free-or-evicted slot; kNoSlot (and a refusal) when nothing is evictable.
    [[nodiscard]] std::size_t insert_slot(VirtualItemId key) noexcept;

    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    std::size_t selected_count_ = 0;
    std::uint64_t clock_ = 0;
    std::uint64_t selection_revision_ = 0;
    SelectionChange last_selection_change_{};
    VirtualItemId focused_key_{};
    bool has_focus_ = false;
    std::vector<Entry> entries_;
    std::vector<std::size_t> buckets_;
    mutable Stats stats_; // peek() is const but still records hits/misses
};

} // namespace tessera::ui::virtualization
