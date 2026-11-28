// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/layer_promotion.hpp"

#include <algorithm>

namespace tessera::ui::paint {
namespace {

constexpr std::uint64_t kEpochMix = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kGenerationMix = 0xBF58476D1CE4E5B9ull;
constexpr std::uint64_t kTableMix = 0x94D049BB133111EBull;

} // namespace

LayerPromotion::LayerPromotion(LayerPromotionConfig config)
    : config_(config) {
    config_.max_entries = std::max<std::uint32_t>(config_.max_entries, 1u);
    entries_.reserve(config_.max_entries);
}

bool LayerPromotion::qualifies(const Entry& entry, std::uint64_t epoch) const {
    return entry.eligible && entry.epoch == epoch && entry.stable >= config_.min_stable_frames;
}

bool LayerPromotion::promotes(NodeId node, std::uint64_t epoch) const {
    const auto found = entries_.find(node);
    return found != entries_.end() && qualifies(found->second, epoch);
}

std::uint64_t LayerPromotion::key_epoch(NodeId node, std::uint64_t epoch) const {
    const auto found = entries_.find(node);
    const std::uint64_t generation = found != entries_.end() ? found->second.generation : 0u;
    return epoch * kEpochMix + (generation + 1u) * kGenerationMix + table_epoch_ * kTableMix;
}

void LayerPromotion::observe(NodeId node, const LayerPromotionObservation& observation) {
    auto found = entries_.find(node);
    if (found == entries_.end()) {
        if (entries_.size() >= config_.max_entries) {
            // Cleared rather than grown: the reserved buckets stay, and every promotion is
            // re-earned. The table epoch keeps a layer recorded before the reset from matching.
            entries_.clear();
            ++table_epoch_;
            ++stats_.table_resets;
        }
        found = entries_.emplace(node, Entry{}).first;
        found->second.epoch = observation.epoch;
    }
    Entry& entry = found->second;
    const bool was_promoted = entry.promoted;
    if (entry.epoch != observation.epoch) {
        entry.epoch = observation.epoch;
        entry.stable = 0;
    }
    if (observation.clean) {
        entry.stable = std::min<std::uint32_t>(entry.stable + 1u, 1u << 20);
    } else {
        entry.stable = 0;
        ++entry.generation;
    }
    entry.draws = observation.draws;
    entry.eligible = !observation.backdrop && !observation.nested_layer &&
                     observation.draws >= config_.min_draws && observation.area >= config_.min_area;
    if (observation.backdrop) {
        ++stats_.rejected_backdrop;
    } else if (observation.nested_layer) {
        ++stats_.rejected_nested;
    } else if (!entry.eligible) {
        ++stats_.rejected_small;
    }
    entry.promoted = qualifies(entry, observation.epoch);
    if (entry.promoted && !was_promoted) {
        ++stats_.promotions;
    } else if (!entry.promoted && was_promoted) {
        ++stats_.demotions;
    }
}

void LayerPromotion::clear() {
    entries_.clear();
    ++table_epoch_;
}

LayerPromotionStats LayerPromotion::stats() const {
    LayerPromotionStats out = stats_;
    out.entries = static_cast<std::uint32_t>(entries_.size());
    for (const auto& [id, entry] : entries_) {
        out.promotable += entry.promoted ? 1u : 0u;
    }
    return out;
}

} // namespace tessera::ui::paint
