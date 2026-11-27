// SPDX-License-Identifier: MIT
//
// The retained-layer cache (issue #617, docs/design/gpu/offscreen-layers.md section 5.6). Entries
// are few (the cap is tens at most) and looked up by owner, so a reserved vector scanned linearly
// beats a map: nothing allocates on the frame path once the slots exist, and a lookup touches one
// cache line per entry.

#include "tessera/rendering/gpu/layer_retention.hpp"

#include <algorithm>
#include <utility>

namespace tessera::rendering::gpu {
namespace {

constexpr std::uint32_t kEntryLimit = 64;

} // namespace

RetainedLayerCache::RetainedLayerCache(OffscreenTargetPool* pool, RetainedLayerConfig config)
    : pool_(pool),
      config_(config) {
    config_.max_entries = std::min(config_.max_entries, kEntryLimit);
    entries_.reserve(config_.max_entries);
}

RetainedLayerCache::Entry* RetainedLayerCache::entry_of(std::uint64_t owner) {
    for (Entry& entry : entries_) {
        if (entry.owner == owner) {
            return &entry;
        }
    }
    return nullptr;
}

void RetainedLayerCache::release(std::size_t index, bool immediate) {
    Entry& entry = entries_[index];
    bytes_ -= std::min(bytes_, entry.bytes);
    if (immediate) {
        pool_->refund(entry.bytes);
        entry.target.reset();
    } else {
        pool_->retire(std::move(entry.target), entry.last_used, entry.bytes);
    }
    // Order is irrelevant, so the hole is filled from the back instead of shifting the tail.
    if (index + 1 != entries_.size()) {
        entries_[index] = std::move(entries_.back());
    }
    entries_.pop_back();
}

void RetainedLayerCache::begin_frame(std::uint64_t frame_index, rhi::Extent2D frame_extent) {
    frame_index_ = frame_index;
    if (!enabled()) {
        return;
    }
    // A reservation the last frame never finished (a layer that was cut short) has no pixels.
    for (std::size_t index = entries_.size(); index-- > 0;) {
        if (!entries_[index].ready) {
            release(index, false);
        }
    }
    const rhi::TextureFormat format = pool_->format();
    if (frame_extent.width != frame_extent_.width || frame_extent.height != frame_extent_.height ||
        format != format_) {
        stats_.evicted += entries_.size();
        while (!entries_.empty()) {
            release(entries_.size() - 1, false);
        }
        frame_extent_ = frame_extent;
        format_ = format;
        return;
    }
    if (config_.idle_frames == 0) {
        return;
    }
    for (std::size_t index = entries_.size(); index-- > 0;) {
        if (frame_index_ - entries_[index].last_used >= config_.idle_frames) {
            ++stats_.evicted;
            release(index, false);
        }
    }
}

RetainedLayerCache::Hit RetainedLayerCache::find(std::uint64_t owner, std::uint64_t key) {
    Entry* entry = entry_of(owner);
    if (entry == nullptr || !entry->ready || entry->key != key) {
        return {};
    }
    entry->last_used = frame_index_;
    ++stats_.hits;
    return Hit{entry->target.get(), entry->region};
}

bool RetainedLayerCache::evict_one() {
    std::size_t victim = entries_.size();
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].last_used == frame_index_) {
            continue;
        }
        if (victim == entries_.size() || entries_[index].last_used < entries_[victim].last_used) {
            victim = index;
        }
    }
    if (victim == entries_.size()) {
        return false;
    }
    ++stats_.evicted;
    release(victim, false);
    return true;
}

rhi::RenderTarget* RetainedLayerCache::reserve(std::uint64_t owner, std::uint64_t key,
                                               const rhi::Scissor& region) {
    if (!enabled() || region.width == 0 || region.height == 0) {
        return nullptr;
    }
    // The owner's previous pixels are dead the moment new ones are asked for.
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].owner == owner) {
            ++stats_.invalidated;
            release(index, false);
            break;
        }
    }
    const rhi::Extent2D extent{region.width, region.height};
    const std::uint64_t bytes = pool_->bytes_for(extent);
    if (config_.max_bytes != 0 && bytes > config_.max_bytes) {
        ++stats_.refused;
        return nullptr;
    }
    while (entries_.size() >= config_.max_entries ||
           (config_.max_bytes != 0 && bytes_ + bytes > config_.max_bytes)) {
        if (!evict_one()) {
            ++stats_.refused;
            return nullptr;
        }
    }
    if (!pool_->charge(bytes)) {
        ++stats_.refused;
        return nullptr;
    }
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = pool_->format();
    desc.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    Entry entry;
    entry.owner = owner;
    entry.key = key;
    entry.target = pool_->device().create_render_target(desc);
    entry.region = region;
    entry.bytes = bytes;
    entry.last_used = frame_index_;
    bytes_ += bytes;
    entries_.push_back(std::move(entry));
    return entries_.back().target.get();
}

void RetainedLayerCache::commit(std::uint64_t owner) {
    if (Entry* entry = entry_of(owner); entry != nullptr && !entry->ready) {
        entry->ready = true;
        ++stats_.records;
    }
}

void RetainedLayerCache::abandon(std::uint64_t owner) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].owner == owner && !entries_[index].ready) {
            release(index, false);
            return;
        }
    }
}

void RetainedLayerCache::invalidate(std::uint64_t owner) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].owner == owner) {
            ++stats_.invalidated;
            release(index, false);
            return;
        }
    }
}

void RetainedLayerCache::trim(bool all_complete) {
    if (!enabled()) {
        return;
    }
    stats_.evicted += entries_.size();
    while (!entries_.empty()) {
        release(entries_.size() - 1, all_complete);
    }
}

RetainedLayerStats RetainedLayerCache::stats() const {
    RetainedLayerStats out = stats_;
    out.entries = static_cast<std::uint32_t>(entries_.size());
    out.bytes = bytes_;
    return out;
}

} // namespace tessera::rendering::gpu
