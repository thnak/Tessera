// SPDX-License-Identifier: MIT
//
// Generic layer promotion (issue #967, rest of #476; docs/design/gpu/offscreen-layers-retention.md
// section 11). A retained layer (issue #617) exists only for a node that already paints as an
// isolated layer: a mask, a filter or a group opacity. Promotion lets an ordinary container take
// the same path: a subtree that is expensive (many draws), big (area) and has not changed for a
// while is painted into a retained layer, so the frames after it composite one quad instead of
// replaying every draw.
//
// The criteria are deterministic and read no clock: the draws the subtree emitted when it was last
// painted, its area, and the number of consecutive painted frames it was clean. A subtree that
// emitted a glass backdrop is never promoted: a layer cannot carry the pixels behind it.
//
// Promotion is an optimisation only. A node dropped from the table, a node that turns dirty and a
// table that overflows all fall back to a flat paint, which is byte-identical. The pipeline keeps
// it off by default: the measured full paint of a large tree is cheap (section 11 has the numbers),
// so it is opt-in until a scene shows rasterization as the bottleneck.

#pragma once

#include "tessera/ui/scene/node_fwd.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace tessera::ui::paint {

struct LayerPromotionConfig {
    // Draws (backgrounds, borders, text runs, images, ...) a subtree must emit to be worth a layer.
    std::uint32_t min_draws = 32;
    // Consecutive painted frames a subtree must be clean before it is promoted.
    std::uint32_t min_stable_frames = 3;
    // Painted area in layout pixels; a small subtree costs more to composite than to replay.
    float min_area = 16384.0f;
    // Nodes tracked at once. The table is reserved up front; when it is full it is cleared (every
    // promotion is dropped and re-earned) instead of growing on the paint path.
    std::uint32_t max_entries = 1024;
};

// What one painted subtree looked like, reported by the paint pass after its children.
struct LayerPromotionObservation {
    std::uint32_t draws = 0;
    float area = 0.0f;
    bool backdrop = false;     // the subtree emitted a glass backdrop
    bool nested_layer = false; // a node inside has its own layer (mask, filter, group opacity)
    bool clean = false;        // the node and its whole subtree were Dirty::Clean
    std::uint64_t epoch = 0;
};

struct LayerPromotionStats {
    std::uint32_t entries = 0;
    std::uint32_t promotable = 0;        // entries that currently meet every criterion
    std::uint64_t promotions = 0;        // a subtree started to qualify
    std::uint64_t demotions = 0;         // a qualifying subtree turned dirty or fell below a limit
    std::uint64_t rejected_backdrop = 0; // observations refused because of a glass backdrop
    std::uint64_t rejected_nested = 0;   // observations refused because a layer is already inside
    std::uint64_t rejected_small = 0; // observations refused for too few draws or too little area
    std::uint64_t table_resets = 0;   // the table was full and cleared
};

class LayerPromotion {
public:
    explicit LayerPromotion(LayerPromotionConfig config = {});

    [[nodiscard]] const LayerPromotionConfig& config() const {
        return config_;
    }

    // True when the node has met every criterion at `epoch`. Reads, never writes.
    [[nodiscard]] bool promotes(NodeId node, std::uint64_t epoch) const;

    // The epoch to fold into the node's retained-layer key. It moves whenever the node turned dirty
    // while it was flat (and no sink was asked), so a layer recorded before that can never match
    // the key of the next promotion.
    [[nodiscard]] std::uint64_t key_epoch(NodeId node, std::uint64_t epoch) const;

    void observe(NodeId node, const LayerPromotionObservation& observation);

    // Forgets everything (a tree swap); entries re-earn their promotion.
    void clear();

    [[nodiscard]] LayerPromotionStats stats() const;

private:
    struct Entry {
        std::uint32_t draws = 0;
        std::uint32_t stable = 0;
        std::uint32_t generation = 0;
        std::uint64_t epoch = 0;
        bool eligible = false; // draws, area and backdrop all pass
        bool promoted = false; // last observation qualified
    };

    [[nodiscard]] bool qualifies(const Entry& entry, std::uint64_t epoch) const;

    LayerPromotionConfig config_;
    std::unordered_map<NodeId, Entry> entries_;
    std::uint64_t table_epoch_ = 0;
    LayerPromotionStats stats_;
};

} // namespace tessera::ui::paint
