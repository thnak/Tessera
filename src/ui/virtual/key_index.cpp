// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/key_index.hpp"

#include <algorithm>
#include <bit>

namespace tessera::ui::virtualization {

namespace {

// Bucket count for a <= 50% load factor, at least one cache-line-ish minimum.
[[nodiscard]] std::size_t bucket_count_for(std::size_t capacity) noexcept {
    const std::size_t wanted = std::max<std::size_t>(capacity * 2, 8);
    return std::bit_ceil(wanted);
}

} // namespace

KeyIndex::KeyIndex(std::size_t capacity)
    : capacity_(capacity),
      keys_(capacity),
      buckets_(bucket_count_for(capacity), 0) {}

std::expected<void, KeyIndexError> KeyIndex::rebuild(const VirtualDataSource& source) {
    const std::size_t count = source.item_count();
    if (count > capacity_) {
        clear();
        return std::unexpected(
            KeyIndexError{KeyIndexError::Kind::CapacityExceeded, VirtualItemId{}, count});
    }
    for (std::size_t index = 0; index < count; ++index) {
        keys_[index] = source.item_key(index);
    }
    return index_keys(count);
}

std::expected<void, KeyIndexError> KeyIndex::rebuild(std::span<const VirtualItemId> keys) {
    if (keys.size() > capacity_) {
        clear();
        return std::unexpected(
            KeyIndexError{KeyIndexError::Kind::CapacityExceeded, VirtualItemId{}, keys.size()});
    }
    std::copy(keys.begin(), keys.end(), keys_.begin());
    return index_keys(keys.size());
}

std::expected<void, KeyIndexError> KeyIndex::index_keys(std::size_t count) {
    clear();
    const std::size_t mask = buckets_.size() - 1;
    for (std::size_t index = 0; index < count; ++index) {
        const VirtualItemId key = keys_[index];
        std::size_t bucket = bucket_index(key);
        // Open addressing: stop at the first empty slot or the key itself. A duplicate is the
        // first repeated key in index order, which keeps the reported error deterministic.
        while (buckets_[bucket] != 0) {
            if (keys_[buckets_[bucket] - 1] == key) {
                clear();
                return std::unexpected(
                    KeyIndexError{KeyIndexError::Kind::DuplicateKey, key, index});
            }
            bucket = (bucket + 1) & mask;
        }
        buckets_[bucket] = index + 1;
    }
    valid_ = true;
    size_ = count;
    return {};
}

std::optional<std::size_t> KeyIndex::find(VirtualItemId key) const noexcept {
    if (!valid_) {
        return std::nullopt;
    }
    const std::size_t bucket = probe(key);
    if (bucket == kEmptyBucket) {
        return std::nullopt;
    }
    return buckets_[bucket] - 1;
}

VirtualItemId KeyIndex::key_at(std::size_t index) const noexcept {
    return index < size_ ? keys_[index] : VirtualItemId{};
}

void KeyIndex::clear() noexcept {
    std::fill(buckets_.begin(), buckets_.end(), 0);
    size_ = 0;
    valid_ = false;
}

std::size_t KeyIndex::bucket_index(VirtualItemId key) const noexcept {
    return static_cast<std::size_t>(hash_virtual_item_id(key)) & (buckets_.size() - 1);
}

std::size_t KeyIndex::probe(VirtualItemId key) const noexcept {
    const std::size_t mask = buckets_.size() - 1;
    for (std::size_t bucket = bucket_index(key);; bucket = (bucket + 1) & mask) {
        if (buckets_[bucket] == 0) {
            return kEmptyBucket;
        }
        if (keys_[buckets_[bucket] - 1] == key) {
            return bucket;
        }
    }
}

} // namespace tessera::ui::virtualization
