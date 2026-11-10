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
        // Focus/selection are store-owned: the payload copy must not move them.
        const bool focused = entries_[existing].state.focused;
        const bool selected = entries_[existing].state.selected;
        entries_[existing].state = state;
        entries_[existing].state.focused = focused;
        entries_[existing].state.selected = selected;
        entries_[existing].stamp = ++clock_;
        return false;
    }
    const std::size_t slot = insert_slot(key);
    if (slot == kNoSlot) {
        return false;
    }
    entries_[slot].state = state;
    entries_[slot].state.focused = false;
    entries_[slot].state.selected = false;
    return true;
}

bool ItemStateStore::set_focus(VirtualItemId key) noexcept {
    std::size_t slot = find_slot(key);
    if (slot == kNoSlot) {
        slot = insert_slot(key);
        if (slot == kNoSlot) {
            return false;
        }
    }
    if (has_focus_ && focused_key_ != key) {
        if (const std::size_t previous = find_slot(focused_key_); previous != kNoSlot) {
            entries_[previous].state.focused = false;
        }
    }
    has_focus_ = true;
    focused_key_ = key;
    entries_[slot].state.focused = true;
    entries_[slot].stamp = ++clock_;
    return true;
}

bool ItemStateStore::clear_focus() noexcept {
    if (!has_focus_) {
        return false;
    }
    if (const std::size_t slot = find_slot(focused_key_); slot != kNoSlot) {
        entries_[slot].state.focused = false;
    }
    has_focus_ = false;
    focused_key_ = VirtualItemId{};
    return true;
}

bool ItemStateStore::set_selected(VirtualItemId key, bool selected) noexcept {
    std::size_t slot = find_slot(key);
    if (!selected) {
        if (slot == kNoSlot || !entries_[slot].state.selected) {
            return false;
        }
        entries_[slot].state.selected = false;
        --selected_count_;
        return true;
    }
    if (slot == kNoSlot) {
        slot = insert_slot(key);
        if (slot == kNoSlot) {
            return false;
        }
    }
    if (!entries_[slot].state.selected) {
        entries_[slot].state.selected = true;
        ++selected_count_;
    }
    entries_[slot].stamp = ++clock_;
    return true;
}

bool ItemStateStore::toggle_selected(VirtualItemId key) noexcept {
    const std::size_t slot = find_slot(key);
    const bool currently = slot != kNoSlot && entries_[slot].state.selected;
    (void)set_selected(key, !currently);
    const std::size_t after = find_slot(key);
    return after != kNoSlot && entries_[after].state.selected;
}

bool ItemStateStore::is_selected(VirtualItemId key) const noexcept {
    const std::size_t slot = find_slot(key);
    return slot != kNoSlot && entries_[slot].state.selected;
}

bool ItemStateStore::clear_selection() noexcept {
    if (selected_count_ == 0) {
        return false;
    }
    for (std::size_t slot = 0; slot < size_; ++slot) {
        entries_[slot].state.selected = false;
    }
    selected_count_ = 0;
    return true;
}

std::size_t ItemStateStore::copy_selected(std::span<VirtualItemId> out) const noexcept {
    std::size_t written = 0;
    for (std::size_t slot = 0; slot < size_ && written < out.size(); ++slot) {
        if (entries_[slot].state.selected) {
            out[written++] = entries_[slot].key;
        }
    }
    return written;
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
    selected_count_ = 0;
    has_focus_ = false;
    focused_key_ = VirtualItemId{};
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
    if (entries_[slot].state.selected) {
        --selected_count_;
    }
    if (has_focus_ && entries_[slot].key == focused_key_) {
        has_focus_ = false;
        focused_key_ = VirtualItemId{};
    }
    const std::size_t last = size_ - 1;
    if (slot != last) {
        entries_[slot] = entries_[last];
        const std::size_t moved_bucket = find_bucket(entries_[slot].key);
        if (moved_bucket != kNoSlot) {
            buckets_[moved_bucket] = slot;
        }
    }
    entries_[last] = Entry{};
    --size_;
    stats_.entries = size_;
}

std::size_t ItemStateStore::lru_victim() const noexcept {
    // Focused and selected entries are pinned: dropping either would silently move the user's
    // focus or shorten the selection (spec section 7).
    std::size_t victim = kNoSlot;
    for (std::size_t slot = 0; slot < size_; ++slot) {
        if (entries_[slot].state.focused || entries_[slot].state.selected) {
            continue;
        }
        if (victim == kNoSlot || entries_[slot].stamp < entries_[victim].stamp) {
            victim = slot;
        }
    }
    return victim;
}

std::size_t ItemStateStore::insert_slot(VirtualItemId key) noexcept {
    if (size_ == capacity_) {
        const std::size_t victim = lru_victim();
        if (victim == kNoSlot) {
            // Every entry is pinned or selected: the capacity rule refuses the insert instead of
            // evicting focus/selection. The caller reports the refusal.
            ++stats_.refusals;
            return kNoSlot;
        }
        erase_slot(victim);
        ++stats_.evictions;
    }
    const std::size_t slot = size_;
    entries_[slot].key = key;
    entries_[slot].state = ItemState{};
    entries_[slot].stamp = ++clock_;
    bucket_insert(bucket_index(key), slot);
    ++size_;
    ++stats_.inserts;
    stats_.entries = size_;
    stats_.peak_entries = std::max(stats_.peak_entries, size_);
    return slot;
}

} // namespace tessera::ui::virtualization
