// SPDX-License-Identifier: MIT
//
// The layer line of the diagnostics overlay (issue #1014,
// docs/design/gpu/offscreen-layers-retention.md section 11): the retained-layer cache of the
// renderer and the promotion policy of the frame pipeline, folded into the plain `OverlayFrame` the
// overlay model reads. Header-only and pure, so the app side that owns both objects fills the line
// in one call and tests need no device.

#pragma once

#include "tessera/diagnostics/overlay.hpp"
#include "tessera/rendering/gpu/layer_retention.hpp"
#include "tessera/ui/paint/layer_promotion.hpp"

namespace tessera::app {

// `cache_enabled` is false for a renderer without a retained-layer cache (or with
// `layerCacheBudgetMb` 0); the overlay then omits the line. `layers_promoted` is the number of
// containers the policy currently promotes (`LayerPromotionStats::promotable`), 0 while
// `layerPromotion` is off.
inline void fill_layer_overlay(diagnostics::OverlayFrame& frame, bool cache_enabled,
                               const rendering::gpu::RetainedLayerStats& cache,
                               const ui::paint::LayerPromotionStats& promotion) noexcept {
    frame.layers_enabled = cache_enabled;
    frame.layer_entries = cache.entries;
    frame.layer_bytes = cache.bytes;
    frame.layer_hits = cache.hits;
    frame.layer_records = cache.records;
    frame.layer_refused = cache.refused;
    frame.layer_evicted = cache.evicted;
    frame.layers_promoted = promotion.promotable;
}

} // namespace tessera::app
