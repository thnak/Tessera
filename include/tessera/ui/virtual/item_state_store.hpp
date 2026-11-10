// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/virtual_item.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace tessera::ui::virtualization {

// Per-item state owned by the collection (`docs/specs/virtualization-v1.md` sections 2 and 3).
//
// Nodes are recycled and hold no item state: everything that must survive scrolling lives here,
// keyed by the stable `VirtualItemId` from the read-only data source. The store never interprets
// the payload; the component maps its own state (selection, draft text, async status, ...) onto
// `payload`/`revision` and keeps the last measured size in `measured_main`.
struct ItemState {
    std::uint64_t revision = 0; // data revision this state was produced from
    std::uint64_t payload = 0;  // component-defined bits or handle
    float measured_main = 0.0f; // last measured main-axis size; 0 = not measured
    float measured_cross = 0.0f;
};

// Bounded keyed store with a deterministic LRU eviction rule.
//
// Storage is allocated once in the constructor: a fixed-capacity dense entry array plus an
// open-addressed bucket index at <= 50% load, so `find`, `peek`, `put` and `erase` never allocate.
// Recency is a monotonic counter bumped on every successful `find`/`put` (no wall clock, no RNG).
// When a `put` of a new key would exceed capacity, the entry with the smallest stamp is evicted;
// overwriting an existing key never evicts. Evictions are counted in `stats()`, so a component can
// refetch data for an evicted key. This is the documented capacity/eviction rule: capacity is a
// constructor argument (default 256) and eviction is always LRU, never growth.
//
// Pointers returned by `find` stay valid until the next `put` of a new key (which may evict),
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

    // Inserts or overwrites. Returns true when the key was newly inserted and false on overwrite.
    bool put(VirtualItemId key, const ItemState& state) noexcept;

    bool erase(VirtualItemId key) noexcept;
    void clear() noexcept;

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
    void evict_lru() noexcept;

    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    std::uint64_t clock_ = 0;
    std::vector<Entry> entries_;
    std::vector<std::size_t> buckets_;
    mutable Stats stats_; // peek() is const but still records hits/misses
};

} // namespace tessera::ui::virtualization
