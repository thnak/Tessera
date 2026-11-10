// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/virtual/virtual_data_source.hpp"
#include "tessera/ui/virtual/virtual_item.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace tessera::ui::virtualization {

// Failure of a key-index rebuild. `index` is the offending item index (the first duplicate, or the
// first item that does not fit the capacity).
struct KeyIndexError {
    enum class Kind {
        CapacityExceeded,
        DuplicateKey,
    };

    Kind kind = Kind::CapacityExceeded;
    VirtualItemId key{};
    std::size_t index = 0;
};

// Bounded key -> index resolution for a read-only data source (spec sections 3 and 6).
//
// The source keys are snapshotted into a dense `keys_` array in index order plus an open-addressed
// bucket table at <= 50% load, both allocated once in the constructor. `find`/`key_at` are O(1) and
// allocation-free, so scroll-to-key, anchoring, and focus restoration can resolve a key on the hot
// path. There is no wall clock and no random seed: the splitmix64 hash fixes the bucket for a key,
// so the probe order is identical on every run and platform.
//
// A rebuild is all-or-nothing from the caller's point of view: capacity overflow or a duplicate
// key clears the index (size() becomes 0 and every lookup misses) and returns the error, so a
// partially built mapping can never answer a query. Callers rebuild after a data-source update and
// keep the previous item count/geometry only if the rebuild succeeded.
class KeyIndex {
public:
    // `capacity` is the hard cap on item count; buckets are sized for a 50% load factor.
    explicit KeyIndex(std::size_t capacity);

    // Snapshots every key from `source`. An empty result means the index is usable.
    [[nodiscard]] std::expected<void, KeyIndexError> rebuild(const VirtualDataSource& source);
    // Direct form used by components that already hold a key array (and by tests).
    [[nodiscard]] std::expected<void, KeyIndexError> rebuild(std::span<const VirtualItemId> keys);

    // Resolves a stable key to its current index; `std::nullopt` for an unknown key or after a
    // failed rebuild.
    [[nodiscard]] std::optional<std::size_t> find(VirtualItemId key) const noexcept;
    [[nodiscard]] bool contains(VirtualItemId key) const noexcept {
        return find(key).has_value();
    }
    // Key at `index` in the last successful snapshot; `VirtualItemId{}` when out of range.
    [[nodiscard]] VirtualItemId key_at(std::size_t index) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

private:
    static constexpr std::size_t kEmptyBucket = static_cast<std::size_t>(-1);

    void clear() noexcept;
    [[nodiscard]] std::expected<void, KeyIndexError> index_keys(std::size_t count);
    [[nodiscard]] std::size_t bucket_index(VirtualItemId key) const noexcept;
    // Bucket holding `key`, or kEmptyBucket.
    [[nodiscard]] std::size_t probe(VirtualItemId key) const noexcept;

    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    bool valid_ = false;
    std::vector<VirtualItemId> keys_;
    // Bucket -> index + 1 so 0 can mean empty; the table has no tombstones (a rebuild starts from a
    // cleared table), so a probe stops at the first empty bucket.
    std::vector<std::size_t> buckets_;
};

} // namespace tessera::ui::virtualization
