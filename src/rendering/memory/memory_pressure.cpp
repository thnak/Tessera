// SPDX-License-Identifier: MIT

#include "tessera/rendering/memory/memory_pressure.hpp"

#include <algorithm>
#include <array>

namespace tessera::rendering::memory {

namespace {

// The documented order (issue #705, owner decision): largest-first strictly.
//
//   image_texture  64 MiB default - re-decodable from the source asset
//   path_texture   32 MiB default - re-rasterizable from the path
//   paint           2 MiB default - recomputed from the computed style
//   glyph_atlas                  - re-rasterized; the most expensive rebuild in the tree
//   text_measure                 - recomputed, but the cheapest of the five
//
// The two texture caches lead because they are both the largest and the cheapest to rebuild, so the
// first step frees the most bytes for the least work. The glyph atlas is fourth rather than last
// because it is larger than the measure cache; the measure cache is last because recomputing a
// measurement is cheaper than re-rasterizing a glyph.
constexpr std::array<std::string_view, 5> kEvictionOrder = {
    "image_texture", "path_texture", "paint", "glyph_atlas", "text_measure",
};

} // namespace

std::span<const std::string_view> MemoryPressure::eviction_order() noexcept {
    return kEvictionOrder;
}

std::size_t eviction_rank(std::string_view name) noexcept {
    const auto found = std::find(kEvictionOrder.begin(), kEvictionOrder.end(), name);
    if (found == kEvictionOrder.end()) {
        return kEvictionOrder.size();
    }
    return static_cast<std::size_t>(std::distance(kEvictionOrder.begin(), found));
}

void MemoryPressure::add(EvictableCache cache) {
    caches_.push_back(std::move(cache));
}

PressureResult MemoryPressure::relieve() {
    PressureResult result;

    // Sort by the documented rank, then by registration order within a rank. A stable sort is what
    // makes the second key work: two caches with the same name (or two unordered ones) keep the
    // order they were registered in, so the sequence is total and reproducible.
    std::vector<std::size_t> order(caches_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [this](std::size_t left, std::size_t right) {
        return eviction_rank(caches_[left].name) < eviction_rank(caches_[right].name);
    });

    for (const std::size_t index : order) {
        EvictableCache& cache = caches_[index];
        if (!cache.evict_idle) {
            continue;
        }
        result.released_bytes += cache.evict_idle();
        ++result.caches_evicted;
    }

    // A retry that cannot succeed only burns the one attempt section 7 allows, so it is offered
    // only when something was actually released.
    result.should_retry = result.released_bytes > 0;
    return result;
}

std::uint64_t MemoryPressure::total_live_bytes() const {
    std::uint64_t total = 0;
    for (const EvictableCache& cache : caches_) {
        if (cache.live_bytes) {
            total += cache.live_bytes();
        }
    }
    return total;
}

} // namespace tessera::rendering::memory
