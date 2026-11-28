// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <vector>

namespace tessera::ui::paint {

// Retained draw list per clean subtree (issue #476 slice 1, dirty-rendering.md section 5.1).
//
// The per-node PaintCache still walks every node of a clean tree and replays each node's record.
// This store sits above it: when a node and everything under it is clean (`Node::subtree_dirty()`
// is Clean; every paint-visible mutation marks Paint on the node and its ancestors), the whole
// subtree's sink calls, pops included, are kept as one flat op list. The next paint with the same
// root key replays that list and never visits the subtree.
//
// The root key is the node's PaintCacheSignature (its own geometry, sink-space mapping, clip stack,
// inherited opacity and context) plus the cull rect, because a culled paint emits fewer draws than
// an unculled one. Descendants are covered by the dirty contract, the same one the per-node cache
// relies on. A replay is byte-identical to re-walking the subtree; dropping the store (budget 0,
// invalidate) can never change the output.
//
// Bounded by entries and bytes with deterministic LRU eviction, like PaintCache. A subtree whose
// list is too short to beat the walk is stored as a tombstone (no ops) so it is not re-recorded on
// every frame.
struct SubtreePaintCacheConfig {
    std::size_t max_entries = 512;
    std::size_t max_bytes = 32 * 1024 * 1024; // runtime config `drawListCacheBudgetMb`
};

// The counters a replay adds back, so a replayed subtree reports the draw composition of a walked
// one. `nodes` is deliberately absent: a replay resolves no style, like a per-node hit.
struct SubtreeStatsDelta {
    PaintCounts counts;
    std::size_t hidden_subtrees = 0;
    std::size_t culled_nodes = 0;
    std::size_t culled_subtrees = 0;
    std::size_t layers = 0;
};

struct SubtreeRecord {
    std::vector<PaintOp> ops; // empty for a tombstone
    std::vector<GradientPayload> gradients;
    SubtreeStatsDelta stats;
    bool tombstone = false;
};

struct SubtreePaintCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t replayed_ops = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

class SubtreePaintCache {
public:
    explicit SubtreePaintCache(SubtreePaintCacheConfig config = {});

    SubtreePaintCache(const SubtreePaintCache&) = delete;
    SubtreePaintCache& operator=(const SubtreePaintCache&) = delete;

    // The record for this root key, or nullptr. The pointer is valid until the next insert/clear.
    [[nodiscard]] const SubtreeRecord* find(const PaintCacheSignature& signature,
                                            const std::optional<rhi::Rect>& cull,
                                            const std::optional<PaintData>& payload);

    void insert(PaintCacheSignature signature, const std::optional<rhi::Rect>& cull,
                const std::optional<PaintData>& payload, SubtreeRecord record);

    // Drops the list rooted at `node`: a paint that visits a dirty subtree calls this, so a list
    // recorded before the change can never be replayed once the tree is clean again.
    void erase(NodeId node);
    void clear();
    void set_config(SubtreePaintCacheConfig config);
    [[nodiscard]] bool enabled() const {
        return config_.max_entries > 0 && config_.max_bytes > 0;
    }
    [[nodiscard]] SubtreePaintCacheStats stats() const;

private:
    struct Entry {
        PaintCacheSignature signature;
        std::optional<rhi::Rect> cull;
        std::optional<PaintData> payload;
        SubtreeRecord record;
        std::size_t bytes = 0;
        std::list<NodeId>::iterator lru;
    };

    static std::size_t entry_bytes(const std::optional<PaintData>& payload,
                                   const SubtreeRecord& record);
    void evict_one();

    SubtreePaintCacheConfig config_;
    std::map<NodeId, Entry> entries_;
    std::list<NodeId> lru_;
    std::size_t bytes_ = 0;
    SubtreePaintCacheStats counters_;
};

} // namespace tessera::ui::paint
