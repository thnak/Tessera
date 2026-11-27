// SPDX-License-Identifier: MIT

#include "lcd_text_sink.hpp"
#include "paint_box.hpp"
#include "paint_cache_internal.hpp"
#include "paint_cull.hpp"
#include "paint_filter.hpp"
#include "paint_frame.hpp"
#include "paint_gradient.hpp"
#include "paint_image.hpp"
#include "paint_layer_retention.hpp"
#include "paint_list_marker.hpp"
#include "paint_node_parts.hpp"
#include "paint_properties.hpp"
#include "paint_shadow.hpp"
#include "paint_text.hpp"
#include "paint_text_shadow.hpp"
#include "paint_vector.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/z_order.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace tessera::ui::paint {
namespace {

// The transformed-frame geometry lives in `paint_frame.hpp`; the helpers below use it by name.
using namespace detail;

// Painted state inherited from the parent: the painted world matrix of the parent's content space
// (scroll offset applied), whether any ancestor already paints with an interpolation override, and
// the effective opacity of the ancestors. `opacity` is the product of every ancestor's `opacity`
// (docs/design/paint-pass.md section 14.1): it multiplies each primitive this node emits, so a
// group fades as a whole without an offscreen layer. `interpolated == false` keeps the
// authoritative Node::world_transform() path untouched.
struct PaintScope {
    rhi::Mat3 content_world;
    bool interpolated = false;
    float opacity = 1.0f;
    // Inside an isolated layer (issue #227): text must stay grayscale there, because a dual-source
    // LCD blend into the layer's transparent target would composite wrong.
    bool in_layer = false;
};

// Inline-span children of a Text node are painted as attributed runs by the parent, never as their
// own nodes (issue #229; elements-basic.md section 2.1).
bool is_inline_span_child(const Node& parent, const Node& child) {
    const auto& data = parent.paint_data();
    return data && data->kind == PaintKind::Text && data->has_span(child.id());
}

void paint_node(const Node& node, StyleProvider& styles, PaintSink& sink, PaintStats& stats,
                const PaintContext& context, Frame frame, const PaintScope& scope,
                const detail::ClipScope& clip_scope, const detail::PassState& pass) {
    if (!node.rendered()) {
        ++stats.hidden_subtrees;
        return;
    }

    // The view may replace the node's own transform; inside an overridden subtree descendants must
    // compose from the painted ancestor matrix instead of their authoritative world_transform().
    const std::optional<rhi::Mat3> override =
        context.view != nullptr ? context.view->local_transform(node.id()) : std::nullopt;
    const bool interpolated = scope.interpolated || override.has_value();
    const rhi::Mat3 local = override.value_or(node.transform());
    rhi::Mat3 painted_world;
    if (interpolated) {
        painted_world =
            scope.content_world * rhi::Mat3::translation(node.bounds().x, node.bounds().y) * local;
    } else {
        painted_world = node.world_transform();
    }

    // A node with its own transform opens a new local frame when the sink can apply the matrix; the
    // decision is taken before any emission so a cache hit can replay the recorded push.
    Frame painted_frame = frame;
    bool open_frame = false;
    if (!is_identity(local) && sink.supports_transform()) {
        Frame opened;
        if (painted_world.try_inverse(opened.inverse)) {
            opened.active = true;
            opened.world = painted_world;
            open_frame = true;
            painted_frame = opened;
        }
    }

    const rhi::Rect rect = paint_rect(node, painted_frame, interpolated, painted_world);

    PaintScope child_scope;
    child_scope.interpolated = interpolated;
    child_scope.opacity = scope.opacity;
    child_scope.in_layer = scope.in_layer;
    if (interpolated) {
        child_scope.content_world =
            painted_world * rhi::Mat3::translation(-node.scroll_x(), -node.scroll_y());
    }

    // Paint-pass culling (PaintContext::cull; issue #487, dirty-rendering.md section 6.8): the
    // reach test is style-free, and a node painted in its own transform frame is never culled (its
    // children are expressed relative to the matrix it would push). A dirty node with a cache
    // record is painted too, so its record is refreshed rather than going stale for a later clean
    // paint; the pipeline's damage always covers a dirty node, a caller's rect need not.
    // Only a dirty node that actually has a record can leave a stale one behind (a node with none
    // is simply painted fresh whenever it is next visited), so the first frame of a scene, whose
    // cache is empty, culls as freely as a clean one (issue #257).
    // A retained layer (#617) goes stale the same way: a dirty node that may hold one is never
    // culled (it must be seen to drop its layer), and a dirty subtree is kept only while some layer
    // is held at all. With none held, culling is exactly as without retention.
    const bool records_dirty =
        (pass.retain_layers && pass.cache != nullptr &&
         (node.dirty() != Dirty::Clean || node.subtree_dirty() != Dirty::Clean) &&
         pass.cache->is_retained_owner(node.id())) ||
        (pass.cacheable && node.dirty() != Dirty::Clean && pass.cache->contains(node.id()));
    // A skipped subtree must not hide a dirty descendant's record either; an empty cache has none.
    const bool subtree_records_dirty =
        records_dirty ||
        (pass.cacheable && node.subtree_dirty() != Dirty::Clean &&
         (!pass.cache->empty() || (pass.retain_layers && pass.cache->has_retained_owners())));
    // Subtree cull (issue #735, slice C): one test at the root rejects every node in the subtree.
    if (pass.cull != nullptr &&
        detail::may_skip_subtree(node, painted_world, *pass.cull, interpolated, open_frame,
                                 subtree_records_dirty)) {
        ++stats.culled_nodes;
        ++stats.culled_subtrees;
        return;
    }
    // Read by reference from the provider's cache; copied only when the view overlays this node.
    std::optional<style::ComputedStyle> overlaid_style;
    const style::ComputedStyle* culled_style = nullptr;
    detail::CullVerdict verdict = detail::CullVerdict::Paint;
    if (pass.cull != nullptr && !open_frame && !records_dirty &&
        !detail::reaches_cull(node, painted_world, *pass.cull)) {
        if (!node.children().empty()) {
            // A culled node with children pays the cascade anyway: whether its children can escape
            // its box is a style question (overflow), and so is the opacity they compose with.
            culled_style = &styles.resolve(node);
            if (context.view != nullptr && context.view->overlays(node.id())) {
                overlaid_style.emplace(*culled_style);
                context.view->overlay_style(node.id(), *overlaid_style);
                culled_style = &*overlaid_style;
            }
        }
        verdict = detail::cull_verdict(node, rect, painted_frame, culled_style);
        if (verdict == detail::CullVerdict::Paint && culled_style != nullptr &&
            !overlaid_style.has_value()) {
            // The node is painted after all: own the style, a provider reference may not outlive
            // the recursion.
            overlaid_style.emplace(*culled_style);
            culled_style = &*overlaid_style;
        }
    }
    if (verdict != detail::CullVerdict::Paint) {
        ++stats.culled_nodes;
        if (verdict == detail::CullVerdict::Subtree) {
            ++stats.culled_subtrees;
            return;
        }
        if (verdict == detail::CullVerdict::Node) {
            return;
        }
        child_scope.opacity = scope.opacity * detail::opacity_value(*culled_style);
        for_each_by_z(node.children(), false, [&](const Node& child) {
            if (is_inline_span_child(node, child)) {
                return false;
            }
            paint_node(child, styles, sink, stats, context, painted_frame, child_scope, clip_scope,
                       pass);
            return false;
        });
        return;
    }

    if (pass.cache != nullptr && context.view != nullptr) {
        // A PaintView can change colors, opacity and transforms per frame in a way the key cannot
        // fingerprint, so every node of such a paint takes the uncached path.
        pass.cache->note_bypass();
    }

    // Per-node paint cache. A hit skips style resolution, text measurement and re-emission for this
    // node; children are always walked because their records are their own. A node marked dirty
    // since the paint that recorded it may have a different computed style, which the style-free
    // key cannot see, so it never hits: it re-records and replaces its entry.
    std::optional<PaintCacheSignature> signature;
    if (pass.cacheable) {
        signature = detail::make_paint_cache_signature(
            node, painted_world, painted_frame.inverse, open_frame, rect, interpolated,
            sink.supports_transform(), clip_scope, pass, scope.opacity, sink.supports_layers(),
            scope.in_layer);
        // Retained draw list (issue #476): a clean subtree is replayed as one flat list, or
        // recorded as one when it has none yet. The recording pass re-enters this node with
        // `in_subtree`.
        if (pass.subtree != nullptr && !pass.in_subtree && node.subtree_dirty() != Dirty::Clean) {
            pass.subtree->erase(node.id()); // a list that predates this change must not come back
        }
        if (pass.subtree != nullptr && pass.subtree_replay && !pass.in_subtree &&
            node.dirty() == Dirty::Clean && node.subtree_dirty() == Dirty::Clean &&
            !node.children().empty()) {
            const std::optional<rhi::Rect> cull = detail::cull_key(pass);
            const SubtreeRecord* list = pass.subtree->find(*signature, cull, node.paint_data());
            if (list != nullptr && !list->tombstone) {
                replay_paint_ops(list->ops, list->gradients, sink);
                detail::add_subtree_stats(stats, list->stats);
                return;
            }
            if (list == nullptr) {
                PaintCache::Recorder recorder(sink, true);
                const PaintStats before = stats;
                pass.in_subtree = true;
                paint_node(node, styles, recorder, stats, context, frame, scope, clip_scope, pass);
                pass.in_subtree = false;
                pass.subtree->insert(*signature, cull, node.paint_data(),
                                     detail::make_subtree_record(recorder, stats, before));
                return;
            }
        }
        if (node.dirty() != Dirty::Clean) {
            pass.cache->note_miss();
        } else if (const PaintRecord* record = pass.cache->find(*signature, node.paint_data())) {
            const PaintCounts counts = record->counts;
            const bool pushed_clip = record->pushed_clip;
            const bool pushed_layer = record->pushed_layer;
            const RoundedClip clip = record->clip;
            // The record carries the effective opacity its ops were folded with, so the child walk
            // composites at the same product without resolving this node's style again.
            child_scope.opacity = record->effective_opacity;
            child_scope.in_layer = scope.in_layer || record->pushed_layer;
            replay_paint_ops(record->ops, record->gradients, sink);
            const detail::ClipScope child_clip =
                pushed_clip ? detail::pushed_clip_scope(clip_scope, clip) : clip_scope;
            for_each_by_z(node.children(), false, [&](const Node& child) {
                if (is_inline_span_child(node, child)) {
                    return false;
                }
                paint_node(child, styles, sink, stats, context, painted_frame, child_scope,
                           child_clip, pass);
                return false;
            });
            if (pushed_clip) {
                sink.pop_clip();
            }
            if (pushed_layer) {
                sink.pop_layer();
            }
            if (open_frame) {
                sink.pop_transform();
            }
            detail::add_paint_counts(stats, counts);
            return;
        }
    }

    style::ComputedStyle style =
        overlaid_style.has_value() ? std::move(*overlaid_style) : styles.compute(node);
    if (context.view != nullptr) {
        // Paint-only: the override is applied to this local copy, never to the scene or the
        // caller's style provider.
        context.view->overlay_style(node.id(), style);
    }
    ++stats.nodes;

    // A node with `mask-image` paints its mask AFTER its children, which a per-node record cannot
    // hold (it stores the prefix and pop flags), so it is never per-node recorded; a stale entry
    // from before the mask appeared is dropped so a later clean frame cannot replay it. Its mask
    // rides in the subtree draw list as ops instead (issue #617).
    const float own_opacity = detail::opacity_value(style);
    const bool masked = own_opacity > 0.0f && rect.width > 0.0f && rect.height > 0.0f &&
                        sink.supports_layers() && detail::has_mask_image(style);
    // `filter` (issue #617) isolates the node like a mask: the chain rides in the layer's push.
    rendering::gpu::LayerFilterChain filter;
    if (own_opacity > 0.0f && rect.width > 0.0f && rect.height > 0.0f && sink.supports_layers()) {
        filter = detail::layer_filter_chain(style, detail::current_color(style), context.glass);
    }
    // A node that may keep its pixels (#617) is not per-node recorded either: the sink is asked
    // before the layer opens, and a hit skips everything a record would replay.
    const bool group_candidate = pass.isolate_group_opacity && own_opacity < 1.0f &&
                                 own_opacity > 0.0f && !node.children().empty() &&
                                 rect.width > 0.0f && rect.height > 0.0f && sink.supports_layers();
    const bool retaining = pass.retain_layers && signature.has_value() &&
                           (masked || !filter.empty() || group_candidate);
    const bool layered = masked || !filter.empty() || retaining;
    if (layered && pass.cache != nullptr) {
        pass.cache->erase(node.id());
    }

    // On a miss the node's own emission is captured while it is forwarded to the real sink.
    std::optional<PaintCache::Recorder> recording;
    PaintSink* own = &sink;
    if (signature.has_value() && !layered) {
        recording.emplace(sink);
        own = &*recording;
    }
    const PaintStats before = stats;

    // A node with its own transform opens a new local frame when the sink can apply the matrix.
    if (open_frame) {
        own->push_transform(frame.active ? frame.inverse * painted_world : painted_world);
        ++stats.transforms;
    }

    // Effective opacity: the ancestor product times this node's own `opacity` (both clamped 0..1).
    // It multiplies every primitive this node emits and is what its children inherit, so a group
    // fades as a whole without an offscreen layer (paint-pass.md 14.1); `opacity: 0` still walks.
    // A faded node with children paints as one isolated layer when the context asks for it and the
    // sink can render one (issue #227): its own opacity is applied once at the composite, so the
    // primitives inside carry only the ancestor product. The box is the whole frame for now (the
    // renderer clamps it to the frame, the clip and the damage); a tighter subtree box is a later
    // refinement. `opacity: 0` paints nothing either way and stays on the flat path.
    const bool isolate_group =
        layered || (pass.isolate_group_opacity && own_opacity < 1.0f && own_opacity > 0.0f &&
                    !node.children().empty() && rect.width > 0.0f && rect.height > 0.0f &&
                    own->supports_layers());
    const float effective_opacity = scope.opacity * (isolate_group ? 1.0f : own_opacity);
    child_scope.opacity = effective_opacity;
    child_scope.in_layer = scope.in_layer || isolate_group;
    if (isolate_group) {
        constexpr float kReach = 1.0e6f;
        const rhi::Rect whole_frame{-kReach, -kReach, 2.0f * kReach, 2.0f * kReach};
        const LayerDraw layer{retaining ? detail::retained_layer_box(node, rect) : whole_frame,
                              own_opacity, filter};
        if (retaining && pass.cache != nullptr) {
            pass.cache->note_retained_owner(node.id(), node.dirty() == Dirty::Clean &&
                                                           node.subtree_dirty() == Dirty::Clean);
        }
        if (retaining &&
            detail::push_layer_retained(*own, node, *signature, layer, masked, pass.retain_epoch)) {
            // The sink composites the pixels it holds at the pop: no box, children or mask.
            own->pop_layer();
            if (open_frame) {
                own->pop_transform();
            }
            ++stats.retained_layers;
            return;
        }
        if (!retaining) {
            own->push_layer(layer);
        }
    }

    if (rect.width > 0.0f && rect.height > 0.0f) {
        const rhi::Color current = detail::current_color(style);
        // The backdrop sits beneath everything the node paints, including its box shadow.
        paint_backdrop(style, rect, effective_opacity, context, *own, stats);
        // The outer shadow sits beneath the node's own background, borders and content.
        const std::optional<float> shadow_radius = detail::uniform_radius(style);
        detail::paint_box_shadows(style, rect, shadow_radius.value_or(0.0f), effective_opacity,
                                  current, *own, stats, detail::ShadowLayer::Outer,
                                  context.reduce_effects);
        paint_box_and_border(node, style, rect, effective_opacity, current, context.device_scale,
                             *own, stats, context.reduce_effects, context.background_layers);
    }

    // `overflow` clipping applies to the children (the element's own background/border are painted
    // above). Scroll containers clip their children as well: the children are offset by -scroll via
    // world_transform(), and the clip keeps them inside the scrollport - the node's
    // `scrollport_insets` when set, otherwise the style padding box. The same box drives clamping
    // and hit testing (see scroll-and-gestures-v1.md 2.1).
    const bool scroll_clips = node.scrollable_x() || node.scrollable_y();
    const bool clip_children =
        (detail::clips_children(style) || scroll_clips) && rect.width > 0.0f && rect.height > 0.0f;
    bool pushed_clip = false;
    RoundedClip pushed_clip_value;
    if (clip_children) {
        const std::optional<rhi::Rect> clip_rect =
            scroll_clips ? scrollport_clip(node, style, rect) : detail::padding_box(style, rect);
        if (clip_rect) {
            if (const auto clip =
                    device_clip(detail::corner_clip(style, *clip_rect), painted_frame)) {
                own->push_clip(*clip);
                ++stats.clips;
                pushed_clip = true;
                pushed_clip_value = *clip;
            }
        }
    }
    // Content runs (one active payload per node): after the element's own background/borders and
    // inside its own overflow/scroll clip, before the children (paint order).
    //
    // `text-shadow` (issue #616) wraps the payload emission: the wrapper buffers every run the
    // payload produces - wrapped lines, span slices, the ellipsized line, the preedit - and emits
    // their shadows first, so every shadow of the block sits behind every glyph of it. Installing
    // it here rather than inside `paint_text_lines` keeps that file inside its source-size limit
    // and puts the wrapper outside the LCD tagger, so a shadow copy is never LCD-eligible.
    const detail::TextShadowList text_shadows =
        detail::parse_text_shadows(style, detail::current_color(style));
    if (text_shadows.empty()) {
        detail::paint_text_lines(
            node, style, rect, effective_opacity, *own, stats, context,
            detail::lcd_text_eligible(context, child_scope.in_layer ? 0.0f : effective_opacity,
                                      painted_frame.active, interpolated, painted_world));
    } else {
        detail::TextShadowSink shadowed(*own, text_shadows, effective_opacity,
                                        context.reduce_effects, stats);
        detail::paint_text_lines(
            node, style, rect, effective_opacity, shadowed, stats, context,
            detail::lcd_text_eligible(context, child_scope.in_layer ? 0.0f : effective_opacity,
                                      painted_frame.active, interpolated, painted_world));
        shadowed.flush();
    }
    detail::paint_vector_content(node, style, rect, effective_opacity, *own, stats);
    detail::paint_image_content(node, style, rect, effective_opacity, *own, stats);
    if (rect.width > 0.0f && rect.height > 0.0f) {
        detail::paint_list_marker(node, style, rect, effective_opacity, context, *own, stats);
    }

    // The record is complete here: pops are stored as flags, so the entry never holds child ops and
    // can be committed before the child walk (a child insert may evict this very entry).
    if (recording.has_value()) {
        PaintRecord record = recording->finish();
        record.counts = detail::paint_counts_since(stats, before);
        record.pushed_clip = pushed_clip;
        record.clip = pushed_clip_value;
        record.effective_opacity = effective_opacity;
        pass.cache->insert(std::move(*signature), node.paint_data(), std::move(record));
        signature.reset();
    }

    const detail::ClipScope child_clip =
        pushed_clip ? detail::pushed_clip_scope(clip_scope, pushed_clip_value) : clip_scope;
    for_each_by_z(node.children(), false, [&](const Node& child) {
        if (is_inline_span_child(node, child)) {
            return false;
        }
        paint_node(child, styles, sink, stats, context, painted_frame, child_scope, child_clip,
                   pass);
        return false;
    });
    if (pushed_clip) {
        sink.pop_clip();
    }
    // The mask goes in after the children and the clip pop, still inside the layer and the node's
    // transform, so it covers the node's border box in its own frame. A sink that cannot mask this
    // layer answers false and the group composites unmasked.
    if (masked && sink.begin_layer_mask()) {
        detail::paint_mask_image(style, rect, sink, stats);
        sink.end_layer_mask();
    }
    if (isolate_group) {
        sink.pop_layer();
    }
    if (open_frame) {
        sink.pop_transform();
    }
}

} // namespace

PaintStats paint_scene(const Node& root, StyleProvider& styles, PaintSink& sink,
                       const PaintContext& context) {
    PaintStats stats;
    PaintContext effective_context = context;
    if (effective_context.styles == nullptr) {
        effective_context.styles = &styles;
    }
    detail::PassState pass;
    pass.cache = context.paint_cache;
    pass.cacheable =
        context.paint_cache != nullptr && context.paint_cache->enabled() && context.view == nullptr;
    pass.metrics_source_id = context.text_metrics_source != nullptr
                                 ? context.text_metrics_source->id()
                                 : kStubTextMetricsId;
    pass.device_scale_bits = detail::float_bits(context.device_scale);
    pass.reduce_effects = context.reduce_effects;
    pass.high_contrast = context.high_contrast;
    pass.lcd_text = context.lcd_text;
    pass.isolate_group_opacity = context.isolate_group_opacity;
    pass.retain_layers = context.retain_layers && pass.cacheable;
    pass.retain_epoch = context.retain_epoch;
    pass.glass_max_blur_radius = context.glass.max_blur_radius;
    pass.glass_max_backdrop_area = context.glass.max_backdrop_area;
    pass.cull = context.cull.has_value() ? &*context.cull : nullptr;
    pass.subtree_replay = context.retain_draw_list;
    pass.subtree =
        pass.cacheable && context.subtree_cache != nullptr && context.subtree_cache->enabled()
            ? context.subtree_cache
            : nullptr;
    if (context.canvas.has_value() && root.rendered()) {
        const rhi::Rect rect = detail::paint_rect(root, Frame{}, false, root.world_transform());
        if (rect.width > 0.0f && rect.height > 0.0f) {
            sink.fill_rect(rect, *context.canvas);
            ++stats.backgrounds;
        }
    }
    paint_node(root, styles, sink, stats, effective_context, Frame{}, PaintScope{},
               detail::ClipScope{}, pass);
    return stats;
}

} // namespace tessera::ui::paint
