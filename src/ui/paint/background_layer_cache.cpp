// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/background_layer_cache.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace tessera::ui::paint {
namespace {

constexpr std::size_t kLruNodePointers = 2 * sizeof(void*);

} // namespace

BackgroundLayerCache::BackgroundLayerCache(BackgroundLayerCacheConfig config)
    : config_(config) {}

std::size_t BackgroundLayerCache::entry_bytes() {
    // The cache copies no text and no layer list of its own that it would not otherwise hold: the
    // entry holds one owning pointer plus the parsed value, and the value is the parse result. So
    // the accounting is the entry, its LRU node, and the owning pointer's control block share.
    return sizeof(Entry) + kLruNodePointers;
}

const BackgroundLayerCache::Value& BackgroundLayerCache::get(const style::ComputedStyle& style,
                                                             std::string_view text) {
    // A default-constructed style shares one null identity with every other; it owns no value, so
    // caching it would hand the first value inserted to every empty style.
    std::shared_ptr<const void> owner = style.identity();
    const bool cacheable = enabled() && owner != nullptr;

    if (cacheable) {
        const auto found = entries_.find(reinterpret_cast<std::uintptr_t>(owner.get()));
        if (found != entries_.end()) {
            ++counters_.hits;
            // Splice only moves the existing list node; a hit allocates nothing.
            lru_.splice(lru_.begin(), lru_, found->second.lru);
            return found->second.value;
        }
    }

    ++counters_.misses;
    Value value;
    value.parsed = style::parse_background_layers(text);
    if (!cacheable) {
        scratch_ = std::move(value);
        return scratch_;
    }

    // The identity is held from here, so the key (its address) cannot be reused by another style
    // while the entry lives. A style whose entries are mutated afterwards gets a fresh block from
    // the copy-on-write `mutable_entries`, so a cached parse can never describe later text.
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(owner.get());
    const std::size_t bytes = entry_bytes();
    if (bytes > config_.max_bytes) {
        // The entry alone exceeds the byte budget: answer through scratch instead of breaking the
        // configured bound.
        scratch_ = std::move(value);
        return scratch_;
    }

    auto [inserted, ok] = entries_.try_emplace(key, Entry{});
    (void)ok;
    inserted->second.owner = std::move(owner);
    inserted->second.value = std::move(value);
    inserted->second.bytes = bytes;
    inserted->second.lru = lru_.insert(lru_.begin(), key);
    bytes_ += bytes;
    while ((entries_.size() > config_.max_entries || bytes_ > config_.max_bytes) &&
           lru_.size() > 1) {
        evict_one();
    }
    return inserted->second.value;
}

void BackgroundLayerCache::evict_one() {
    if (lru_.empty()) {
        return;
    }
    const std::uintptr_t victim = lru_.back();
    const auto found = entries_.find(victim);
    if (found != entries_.end()) {
        bytes_ -= found->second.bytes;
        entries_.erase(found);
        ++counters_.evictions;
    }
    lru_.pop_back();
}

void BackgroundLayerCache::invalidate() {
    clear();
    ++counters_.invalidations;
}

void BackgroundLayerCache::clear() {
    entries_.clear();
    lru_.clear();
    bytes_ = 0;
    scratch_ = Value{};
}

void BackgroundLayerCache::set_enabled(bool enabled) {
    enabled_ = enabled;
}

BackgroundLayerCacheStats BackgroundLayerCache::stats() const {
    BackgroundLayerCacheStats result = counters_;
    result.entries = entries_.size();
    result.bytes = bytes_;
    return result;
}

} // namespace tessera::ui::paint
