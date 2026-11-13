// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"

#include <cstdint>
#include <cstring>

// Internal seam between the paint recursion and the per-node paint cache. Not installed: the pass
// owns the recursion state (clip fingerprint, per-paint context inputs) and the cache owns the key
// contract, so the key construction lives next to the cache's comparison instead of inside the
// paint loop, which keeps paint_pass.cpp under the file limit.
namespace tessera::ui::paint::detail {

// Fingerprint of the clip stack inherited by a node. A record is never replayed under a different
// inherited clip even though a node's own draws do not read the ancestor clips: the conservative
// rule is the one paint_cache.hpp documents, and it keeps a clip change from ever reusing a record
// whose children were keyed under the old clip.
struct ClipScope {
    std::uint64_t hash = kPaintClipHashSeed;
    std::uint32_t depth = 0;
};

// Per-paint cache inputs threaded down the recursion instead of recomputing them per node.
struct PassState {
    PaintCache* cache = nullptr;
    bool cacheable = false; // cache != null, enabled, no PaintView installed
    std::uint32_t metrics_source_id = kStubTextMetricsId;
    std::uint32_t device_scale_bits = 0;
    bool reduce_effects = false;
    bool high_contrast = false;
    float glass_max_blur_radius = 0.0f;
    std::uint64_t glass_max_backdrop_area = 0;
};

inline std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Every input of the key is read without resolving style, which is what lets a hit skip style
// resolution entirely (paint_cache.hpp holds the full validity contract). `frame_inverse` is the
// painting frame's inverse when the node opened one, else identity.
inline PaintCacheSignature
make_paint_cache_signature(const Node& node, const rhi::Mat3& painted_world,
                           const rhi::Mat3& frame_inverse, bool frame_active, const rhi::Rect& rect,
                           bool interpolated, bool supports_transform, const ClipScope& clip_scope,
                           const PassState& pass) {
    PaintCacheSignature signature;
    signature.node = node.id();
    signature.bounds = node.bounds();
    signature.transform = node.transform();
    signature.scrollable_x = node.scrollable_x();
    signature.scrollable_y = node.scrollable_y();
    signature.scroll_x = node.scroll_x();
    signature.scroll_y = node.scroll_y();
    signature.scrollport_left = node.scrollport_left();
    signature.scrollport_top = node.scrollport_top();
    signature.scrollport_right = node.scrollport_right();
    signature.scrollport_bottom = node.scrollport_bottom();
    signature.interpolated = interpolated;
    signature.painted_world = painted_world;
    signature.frame_active = frame_active;
    signature.frame_inverse = frame_inverse;
    signature.sink_supports_transform = supports_transform;
    signature.paint_rect = rect;
    signature.clip_hash = clip_scope.hash;
    signature.clip_depth = clip_scope.depth;
    signature.metrics_source_id = pass.metrics_source_id;
    signature.device_scale_bits = pass.device_scale_bits;
    signature.reduce_effects = pass.reduce_effects;
    signature.high_contrast = pass.high_contrast;
    signature.glass_max_blur_radius = pass.glass_max_blur_radius;
    signature.glass_max_backdrop_area = pass.glass_max_backdrop_area;
    return signature;
}

// Counters this node's own emission produced; children keep emitting into `stats` directly.
inline PaintCounts paint_counts_since(const PaintStats& current, const PaintStats& before) {
    PaintCounts counts;
    counts.backgrounds = current.backgrounds - before.backgrounds;
    counts.borders = current.borders - before.borders;
    counts.clips = current.clips - before.clips;
    counts.texts = current.texts - before.texts;
    counts.vectors = current.vectors - before.vectors;
    counts.images = current.images - before.images;
    counts.gradients = current.gradients - before.gradients;
    counts.shadows = current.shadows - before.shadows;
    counts.transforms = current.transforms - before.transforms;
    counts.backdrops = current.backdrops - before.backdrops;
    return counts;
}

// A replayed record emits the same draw composition as a cold paint, so the draw counters are
// restored; only PaintStats::nodes stays lower because no style was resolved for the node.
inline void add_paint_counts(PaintStats& stats, const PaintCounts& counts) {
    stats.backgrounds += counts.backgrounds;
    stats.borders += counts.borders;
    stats.clips += counts.clips;
    stats.texts += counts.texts;
    stats.vectors += counts.vectors;
    stats.images += counts.images;
    stats.gradients += counts.gradients;
    stats.shadows += counts.shadows;
    stats.transforms += counts.transforms;
    stats.backdrops += counts.backdrops;
}

inline ClipScope pushed_clip_scope(const ClipScope& parent, const RoundedClip& clip) {
    ClipScope child = parent;
    child.hash = extend_paint_clip_hash(parent.hash, clip);
    child.depth = parent.depth + 1;
    return child;
}

} // namespace tessera::ui::paint::detail
