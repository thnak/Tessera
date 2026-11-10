// SPDX-License-Identifier: MIT

#include "tessera/ui/virtual/item_state_store.hpp"

#include <algorithm>
#include <bit>

namespace tessera::ui::virtualization {

ItemStateStore::ItemStateStore(std::size_t capacity)
    : capacity_(std::max(capacity, kMinCapacity)),
      entries_(capacity_),
      buckets_(std::bit_ceil(std::max(capacity_ * 2, std::size_t{4})), kEmptyBucket) {}

ItemState* ItemStateStore::find(VirtualItemId key) noexcept {
    const std::size_t slot = find_slot(key);
    if (slot == kNoSlot) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    entries_[slot].stamp = ++clock_;
    return &entries_[slot].state;
}

const ItemState* ItemStateStore::peek(VirtualItemId key) const noexcept {
    const std::size_t slot = find_slot(key);
    if (slot == kNoSlot) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &entries_[slot].state;
}

bool ItemStateStore::contains(VirtualItemId key) const noexcept {
    return find_slot(key) != kNoSlot;
}

bool ItemStateStore::put(VirtualItemId key, const ItemState& state) noexcept {
    const std::size_t existing = find_slot(key);
    if (existing != kNoSlot) {
        entries_[existing].state = state;
        entries_[existing].stamp = ++clock_;
        return false;
    }
    if (size_ == capacity_) {
        evict_lru();
    }
    const std::size_t slot = size_;
    entries_[slot].key = key;
    entries_[slot].state = state;
    entries_[slot].stamp = ++clock_;
    bucket_insert(bucket_index(key), slot);
    ++size_;
    ++stats_.inserts;
    stats_.entries = size_;
    stats_.peak_entries = std::max(stats_.peak_entries, size_);
    return true;
}

bool ItemStateStore::erase(VirtualItemId key) noexcept {
    const std::size_t slot = find_slot(key);
    if (slot == kNoSlot) {
        return false;
    }
    erase_slot(slot);
    return true;
}

void ItemStateStore::clear() noexcept {
    std::fill(buckets_.begin(), buckets_.end(), kEmptyBucket);
    size_ = 0;
    stats_.entries = 0;
}

std::size_t ItemStateStore::bucket_index(VirtualItemId key) const noexcept {
    const std::size_t mask = buckets_.size() - 1;
    return static_cast<std::size_t>(hash_virtual_item_id(key)) & mask;
}

std::size_t ItemStateStore::find_bucket(VirtualItemId key) const noexcept {
    const std::size_t mask = buckets_.size() - 1;
    for (std::size_t bucket = bucket_index(key); buckets_[bucket] != kEmptyBucket;
         bucket = (bucket + 1) & mask) {
        if (entries_[buckets_[bucket]].key == key) {
            return bucket;
        }
    }
    return kNoSlot;
}

std::size_t ItemStateStore::find_slot(VirtualItemId key) const noexcept {
    const std::size_t bucket = find_bucket(key);
    return bucket == kNoSlot ? kNoSlot : buckets_[bucket];
}

void ItemStateStore::bucket_insert(std::size_t bucket, std::size_t slot) noexcept {
    const std::size_t mask = buckets_.size() - 1;
    std::size_t index = bucket;
    while (buckets_[index] != kEmptyBucket) {
        index = (index + 1) & mask;
    }
    buckets_[index] = slot;
}

void ItemStateStore::bucket_remove(std::size_t bucket) noexcept {
    // Backward-shift deletion keeps linear-probe chains intact without tombstones, so the table
    // never fills with dead slots and lookups stay bounded.
    const std::size_t mask = buckets_.size() - 1;
    std::size_t hole = bucket;
    buckets_[hole] = kEmptyBucket;
    for (std::size_t probe = (hole + 1) & mask; buckets_[probe] != kEmptyBucket;
         probe = (probe + 1) & mask) {
        const std::size_t home = bucket_index(entries_[buckets_[probe]].key);
        const bool hole_in_path =
            probe > hole ? (home <= hole || home > probe) : (home <= hole && home > probe);
        if (hole_in_path) {
            buckets_[hole] = buckets_[probe];
            buckets_[probe] = kEmptyBucket;
            hole = probe;
        }
    }
}

void ItemStateStore::erase_slot(std::size_t slot) noexcept {
    const std::size_t bucket = find_bucket(entries_[slot].key);
    if (bucket != kNoSlot) {
        bucket_remove(bucket);
    }
    const std::size_t last = size_ - 1;
    if (slot != last) {
        entries_[slot] = entries_[last];
        const std::size_t moved_bucket = find_bucket(entries_[slot].key);
        if (moved_bucket != kNoSlot) {
            buckets_[moved_bucket] = slot;
        }
    }
    --size_;
    stats_.entries = size_;
}

void ItemStateStore::evict_lru() noexcept {
    std::size_t victim = 0;
    for (std::size_t slot = 1; slot < size_; ++slot) {
        if (entries_[slot].stamp < entries_[victim].stamp) {
            victim = slot;
        }
    }
    erase_slot(victim);
    ++stats_.evictions;
}

} // namespace tessera::ui::virtualization
