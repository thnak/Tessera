// SPDX-License-Identifier: MIT

#pragma once

#include "paint_list_marker.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/paint/subtree_paint_cache.hpp"
#include "tessera/ui/scene/node.hpp"

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
    bool lcd_text = false;
    bool isolate_group_opacity = false;
    // PaintContext::retain_layers (issue #617) and the epoch folded into every retained-layer key.
    bool retain_layers = false;
    std::uint64_t retain_epoch = 0;
    LayerPromotion* promotion = nullptr; // PaintContext::layer_promotion (issue #967)
    float glass_max_blur_radius = 0.0f;
    std::uint64_t glass_max_backdrop_area = 0;
    // PaintContext::cull, world space; null paints everything (paint-pass culling, issue #487).
    const rhi::Rect* cull = nullptr;
    // Retained draw list per clean subtree (issue #476); null when the store is off. `in_subtree`
    // is set while a subtree is being recorded, so the recursion under it records nothing nested.
    SubtreePaintCache* subtree = nullptr;
    bool subtree_replay = false; // PaintContext::retain_draw_list
    mutable bool in_subtree = false;
};

inline std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// The per-paint inputs of one `paint_scene` call, read once from the context.
inline PassState make_pass_state(const PaintContext& context) {
    PassState pass;
    pass.cache = context.paint_cache;
    pass.cacheable =
        context.paint_cache != nullptr && context.paint_cache->enabled() && context.view == nullptr;
    pass.metrics_source_id = context.text_metrics_source != nullptr
                                 ? context.text_metrics_source->id()
                                 : kStubTextMetricsId;
    pass.device_scale_bits = float_bits(context.device_scale);
    pass.reduce_effects = context.reduce_effects;
    pass.high_contrast = context.high_contrast;
    pass.lcd_text = context.lcd_text;
    pass.isolate_group_opacity = context.isolate_group_opacity;
    pass.retain_layers = context.retain_layers && pass.cacheable;
    pass.retain_epoch = context.retain_epoch;
    pass.promotion = context.layer_promotion;
    pass.glass_max_blur_radius = context.glass.max_blur_radius;
    pass.glass_max_backdrop_area = context.glass.max_backdrop_area;
    pass.cull = context.cull.has_value() ? &*context.cull : nullptr;
    pass.subtree_replay = context.retain_draw_list;
    pass.subtree =
        pass.cacheable && context.subtree_cache != nullptr && context.subtree_cache->enabled()
            ? context.subtree_cache
            : nullptr;
    return pass;
}

// Every input of the key is read without resolving style, which is what lets a hit skip style
// resolution entirely (paint_cache.hpp holds the full validity contract). `frame_inverse` is the
// painting frame's inverse when the node opened one, else identity. `inherited_opacity` is the
// effective opacity of the ancestors, the product a record's colours were folded with.
inline PaintCacheSignature
make_paint_cache_signature(const Node& node, const rhi::Mat3& painted_world,
                           const rhi::Mat3& frame_inverse, bool frame_active, const rhi::Rect& rect,
                           bool interpolated, bool supports_transform, const ClipScope& clip_scope,
                           const PassState& pass, float inherited_opacity,
                           bool sink_supports_layers, bool in_layer) {
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
    signature.opacity_bits = float_bits(inherited_opacity);
    signature.metrics_source_id = pass.metrics_source_id;
    signature.device_scale_bits = pass.device_scale_bits;
    signature.reduce_effects = pass.reduce_effects;
    signature.high_contrast = pass.high_contrast;
    signature.lcd_text = pass.lcd_text;
    signature.isolate_groups = pass.isolate_group_opacity && sink_supports_layers;
    signature.in_layer = in_layer;
    signature.glass_max_blur_radius = pass.glass_max_blur_radius;
    signature.glass_max_backdrop_area = pass.glass_max_backdrop_area;
    const ListNumber number = list_item_number(node);
    signature.list_ordinal = number.value;
    signature.list_item = number.item;
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
    counts.text_shadows = current.text_shadows - before.text_shadows;
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
    stats.text_shadows += counts.text_shadows;
    stats.transforms += counts.transforms;
    stats.backdrops += counts.backdrops;
}

// Retained draw list helpers (issue #476). A subtree list shorter than this loses to walking it, so
// it is stored as a tombstone instead.
inline constexpr std::size_t kMinSubtreeOps = 8;

inline std::optional<rhi::Rect> cull_key(const PassState& pass) {
    return pass.cull != nullptr ? std::optional<rhi::Rect>(*pass.cull) : std::nullopt;
}

inline void add_subtree_stats(PaintStats& stats, const SubtreeStatsDelta& delta) {
    add_paint_counts(stats, delta.counts);
    stats.hidden_subtrees += delta.hidden_subtrees;
    stats.culled_nodes += delta.culled_nodes;
    stats.culled_subtrees += delta.culled_subtrees;
    stats.layers += delta.layers;
}

// Turns what a recording pass captured into the stored value; `nodes` is left out on purpose.
inline SubtreeRecord make_subtree_record(PaintCache::Recorder& recorder, const PaintStats& current,
                                         const PaintStats& before) {
    SubtreeRecord record;
    PaintRecord captured = recorder.finish();
    record.stats.counts = paint_counts_since(current, before);
    record.stats.hidden_subtrees = current.hidden_subtrees - before.hidden_subtrees;
    record.stats.culled_nodes = current.culled_nodes - before.culled_nodes;
    record.stats.layers = current.layers - before.layers;
    record.stats.culled_subtrees = current.culled_subtrees - before.culled_subtrees;
    if (captured.ops.size() < kMinSubtreeOps) {
        record.tombstone = true;
        return record;
    }
    record.ops = std::move(captured.ops);
    record.gradients = std::move(captured.gradients);
    return record;
}

inline ClipScope pushed_clip_scope(const ClipScope& parent, const RoundedClip& clip) {
    ClipScope child = parent;
    child.hash = extend_paint_clip_hash(parent.hash, clip);
    child.depth = parent.depth + 1;
    return child;
}

} // namespace tessera::ui::paint::detail
