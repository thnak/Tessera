// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/subtree_paint_cache.hpp"

#include <iterator>
#include <utility>

namespace tessera::ui::paint {
namespace {

bool same_cull(const std::optional<rhi::Rect>& lhs, const std::optional<rhi::Rect>& rhs) {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs.has_value() || (lhs->x == rhs->x && lhs->y == rhs->y && lhs->width == rhs->width &&
                                lhs->height == rhs->height);
}

std::size_t owned_bytes(const PaintOp& op) {
    if (const auto* text = std::get_if<DrawTextOp>(&op.payload)) {
        return text->text.capacity();
    }
    if (const auto* asset = std::get_if<DrawAssetOp>(&op.payload)) {
        return asset->asset.capacity();
    }
    return 0;
}

} // namespace

SubtreePaintCache::SubtreePaintCache(SubtreePaintCacheConfig config)
    : config_(config) {}

void SubtreePaintCache::set_config(SubtreePaintCacheConfig config) {
    config_ = config;
    while (!entries_.empty() &&
           (entries_.size() > config_.max_entries || bytes_ > config_.max_bytes)) {
        evict_one();
    }
}

const SubtreeRecord* SubtreePaintCache::find(const PaintCacheSignature& signature,
                                             const std::optional<rhi::Rect>& cull,
                                             const std::optional<PaintData>& payload) {
    const auto found = entries_.find(signature.node);
    if (found == entries_.end() || !same_paint_signature(found->second.signature, signature) ||
        !same_cull(found->second.cull, cull) || !(found->second.payload == payload)) {
        ++counters_.misses;
        return nullptr;
    }
    lru_.splice(lru_.end(), lru_, found->second.lru);
    if (!found->second.record.tombstone) {
        ++counters_.hits;
        counters_.replayed_ops += found->second.record.ops.size();
    }
    return &found->second.record;
}

void SubtreePaintCache::insert(PaintCacheSignature signature, const std::optional<rhi::Rect>& cull,
                               const std::optional<PaintData>& payload, SubtreeRecord record) {
    if (!enabled()) {
        return;
    }
    const NodeId node = signature.node;
    if (const auto existing = entries_.find(node); existing != entries_.end()) {
        bytes_ -= existing->second.bytes;
        lru_.erase(existing->second.lru);
        entries_.erase(existing);
    }
    const std::size_t bytes = entry_bytes(payload, record);
    while (!entries_.empty() &&
           (entries_.size() + 1 > config_.max_entries || bytes_ + bytes > config_.max_bytes)) {
        evict_one();
    }
    if (entries_.size() + 1 > config_.max_entries || bytes_ + bytes > config_.max_bytes) {
        return;
    }
    Entry entry;
    entry.signature = std::move(signature);
    entry.cull = cull;
    entry.payload = payload;
    entry.record = std::move(record);
    entry.bytes = bytes;
    lru_.push_back(node);
    entry.lru = std::prev(lru_.end());
    bytes_ += bytes;
    entries_.emplace(node, std::move(entry));
}

void SubtreePaintCache::erase(NodeId node) {
    const auto found = entries_.find(node);
    if (found == entries_.end()) {
        return;
    }
    bytes_ -= found->second.bytes;
    lru_.erase(found->second.lru);
    entries_.erase(found);
}

void SubtreePaintCache::clear() {
    entries_.clear();
    lru_.clear();
    bytes_ = 0;
}

void SubtreePaintCache::evict_one() {
    if (lru_.empty()) {
        return;
    }
    const NodeId victim = lru_.front();
    lru_.pop_front();
    if (const auto found = entries_.find(victim); found != entries_.end()) {
        bytes_ -= found->second.bytes;
        entries_.erase(found);
    }
    ++counters_.evictions;
}

std::size_t SubtreePaintCache::entry_bytes(const std::optional<PaintData>& payload,
                                           const SubtreeRecord& record) {
    std::size_t bytes = sizeof(Entry) + sizeof(NodeId) + 2 * sizeof(void*);
    bytes += record.ops.capacity() * sizeof(PaintOp);
    bytes += record.gradients.capacity() * sizeof(GradientPayload);
    if (payload.has_value()) {
        bytes += payload->text.capacity();
        if (payload->decoration.has_value()) {
            bytes += payload->decoration->preedit_text.capacity();
        }
    }
    for (const PaintOp& op : record.ops) {
        bytes += owned_bytes(op);
    }
    return bytes;
}

SubtreePaintCacheStats SubtreePaintCache::stats() const {
    SubtreePaintCacheStats result = counters_;
    result.entries = entries_.size();
    result.bytes = bytes_;
    return result;
}

} // namespace tessera::ui::paint
