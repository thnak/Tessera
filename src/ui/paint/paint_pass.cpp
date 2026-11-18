// SPDX-License-Identifier: MIT

#include "lcd_text_sink.hpp"
#include "paint_cache_internal.hpp"
#include "paint_cull.hpp"
#include "paint_frame.hpp"
#include "paint_image.hpp"
#include "paint_properties.hpp"
#include "paint_shadow.hpp"
#include "paint_text.hpp"
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

void emit_rect(PaintSink& sink, const rhi::Rect& rect, const std::optional<rhi::Color>& color,
               float opacity, std::size_t& counter) {
    if (!color || rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    rhi::Color value = *color;
    value.a *= opacity;
    if (value.a <= 0.0f) {
        return;
    }
    sink.fill_rect(rect, value);
    ++counter;
}

void paint_background(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                      const rhi::Color& current, PaintSink& sink, PaintStats& stats) {
    const std::optional<rhi::Color> color = detail::color_value(style, "background-color", current);
    if (!color || color->a <= 0.0f || opacity <= 0.0f) {
        return;
    }
    rhi::Color value = *color;
    value.a *= opacity;
    if (value.a <= 0.0f) {
        return;
    }

    const std::optional<float> radius = detail::uniform_radius(style);
    if (radius && *radius > 0.0f) {
        sink.fill_rounded_rect(rect, value, clamp_radius(*radius, rect));
    } else {
        sink.fill_rect(rect, value);
    }
    ++stats.backgrounds;
}

void paint_borders(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                   const rhi::Color& current, float device_scale, PaintSink& sink,
                   PaintStats& stats) {
    const detail::BorderWidths widths =
        detail::snap_border_widths(detail::border_widths(style, rect), device_scale);
    const float top = widths.top;
    const float right = widths.right;
    const float bottom = widths.bottom;
    const float left = widths.left;

    const float middle_height = rect.height - top - bottom;
    const std::array<std::optional<rhi::Color>, 4> colors = detail::border_colors(style, current);

    emit_rect(sink, rhi::Rect{rect.x, rect.y, rect.width, top}, colors[0], opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x + rect.width - right, rect.y + top, right, middle_height},
              colors[1], opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + rect.height - bottom, rect.width, bottom}, colors[2],
              opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + top, left, middle_height}, colors[3], opacity,
              stats.borders);
}

// A border on a rounded box. The edge strips are rectangles and cannot follow a corner, so a
// uniform border (one width, one colour, one radius) is drawn as a ring: the rounded box minus the
// same box inset by the width, anti-aliased on both edges by the sink. A border with different
// widths or colours per side keeps the edge strips.
struct RingBorder {
    rhi::Color color;
    float width = 0.0f;
    float radius = 0.0f;
};

bool same_rgba(const rhi::Color& lhs, const rhi::Color& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

std::optional<RingBorder> rounded_ring(const style::ComputedStyle& style, const rhi::Rect& rect,
                                       float opacity, const rhi::Color& current,
                                       float device_scale) {
    const std::optional<float> radius = detail::uniform_radius(style);
    if (!radius || *radius <= 0.0f) {
        return std::nullopt;
    }
    const detail::BorderWidths widths =
        detail::snap_border_widths(detail::border_widths(style, rect), device_scale);
    if (widths.top <= 0.0f || widths.top != widths.right || widths.top != widths.bottom ||
        widths.top != widths.left) {
        return std::nullopt;
    }
    const std::array<std::optional<rhi::Color>, 4> colors = detail::border_colors(style, current);
    if (!colors[0] || !colors[1] || !colors[2] || !colors[3] ||
        !same_rgba(*colors[0], *colors[1]) || !same_rgba(*colors[0], *colors[2]) ||
        !same_rgba(*colors[0], *colors[3])) {
        return std::nullopt;
    }
    rhi::Color color = *colors[0];
    color.a *= opacity;
    if (color.a <= 0.0f) {
        return std::nullopt;
    }
    return RingBorder{color, widths.top, clamp_radius(*radius, rect)};
}

// Glass backdrop (docs/specs/glass-v1.md sections 2, 3 and 5): emitted before the node's own box
// visuals, because what is behind the node at this point in paint order is exactly its backdrop.
// High contrast ignores `backdrop-filter` entirely (section 3.5), so the stylesheet's opaque
// fallback stays visible. The sigma is clamped here, where the configured `glassMaxBlurRadius` is
// known, and the opacity is folded into the composite: with straight-alpha source-over over the
// same region, `opacity * blurred + (1 - opacity) * original` is the correct translucent result.
void paint_backdrop(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                    const PaintContext& context, PaintSink& sink, PaintStats& stats) {
    if (context.high_contrast || rect.width <= 0.0f || rect.height <= 0.0f || opacity <= 0.0f) {
        return;
    }
    const std::optional<style::BackdropFilter> filter = detail::backdrop_filter_value(style);
    if (!filter.has_value()) {
        return;
    }
    const float sigma = rendering::gpu::clamp_glass_sigma(filter->blur_sigma, context.glass);

    BackdropDraw draw;
    draw.rect = rect;
    draw.clip = detail::corner_clip(style, rect);
    draw.sigma = sigma;
    draw.opacity = std::clamp(opacity, 0.0f, 1.0f);
    draw.saturate = filter->saturate;
    draw.brightness = filter->brightness;
    draw.contrast = filter->contrast;
    sink.draw_backdrop(draw);
    ++stats.backdrops;
}

// Linear-gradient background: emitted after the solid background and before borders/content, at
// the node box clipped by the node's own border radius. Stops are clamped to [0,1] and stably
// sorted; equal offsets keep input order (the later stop wins at the duplicate point). Node opacity
// is folded into each stop's alpha here, so sinks never see the node opacity separately.
void paint_linear_gradient(const Node& node, const style::ComputedStyle& style,
                           const rhi::Rect& rect, float opacity, PaintSink& sink,
                           PaintStats& stats) {
    const auto& data = node.paint_data();
    if (!data || !data->gradient || data->gradient->stop_count == 0 || opacity <= 0.0f) {
        return;
    }
    LinearGradient gradient = *data->gradient;
    if (gradient.stop_count > LinearGradient::kMaxStops) {
        gradient.stop_count = LinearGradient::kMaxStops;
    }
    for (std::size_t index = 0; index < gradient.stop_count; ++index) {
        gradient.stops[index].offset = std::clamp(gradient.stops[index].offset, 0.0f, 1.0f);
    }
    std::stable_sort(gradient.stops.begin(), gradient.stops.begin() + gradient.stop_count,
                     [](const LinearGradientStop& lhs, const LinearGradientStop& rhs) {
                         return lhs.offset < rhs.offset;
                     });
    for (std::size_t index = 0; index < gradient.stop_count; ++index) {
        gradient.stops[index].color.a *= opacity;
    }
    GradientDraw draw;
    draw.rect = rect;
    draw.clip = detail::corner_clip(style, rect);
    draw.angle_degrees = gradient.angle_degrees;
    draw.stops = gradient.stops;
    draw.stop_count = gradient.stop_count;
    sink.fill_linear_gradient(draw);
    ++stats.gradients;
}

// The node's own box visuals in paint order: background, gradient, border.
void paint_box_and_border(const Node& node, const style::ComputedStyle& style,
                          const rhi::Rect& rect, float opacity, const rhi::Color& current,
                          float device_scale, PaintSink& sink, PaintStats& stats) {
    paint_background(style, rect, opacity, current, sink, stats);
    paint_linear_gradient(node, style, rect, opacity, sink, stats);
    if (const std::optional<RingBorder> ring =
            rounded_ring(style, rect, opacity, current, device_scale)) {
        sink.fill_rounded_ring(rect, ring->color, ring->radius, ring->width);
        ++stats.borders;
        return;
    }
    paint_borders(style, rect, opacity, current, device_scale, sink, stats);
}

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
    if (interpolated) {
        child_scope.content_world =
            painted_world * rhi::Mat3::translation(-node.scroll_x(), -node.scroll_y());
    }

    // Paint-pass culling (PaintContext::cull; issue #487, dirty-rendering.md section 6.8): the
    // reach test is style-free, and a node painted in its own transform frame is never culled (its
    // children are expressed relative to the matrix it would push). A dirty node with a cache
    // record is painted too, so its record is refreshed rather than going stale for a later clean
    // paint; the pipeline's damage always covers a dirty node, a caller's rect need not.
    const bool records_dirty = pass.cacheable && node.dirty() != Dirty::Clean;
    std::optional<style::ComputedStyle> culled_style;
    detail::CullVerdict verdict = detail::CullVerdict::Paint;
    if (pass.cull != nullptr && !open_frame && !records_dirty &&
        !detail::reaches_cull(node, painted_world, *pass.cull)) {
        if (!node.children().empty()) {
            // A culled node with children pays the cascade anyway: whether its children can escape
            // its box is a style question (overflow), and so is the opacity they compose with.
            culled_style.emplace(styles.compute(node));
            if (context.view != nullptr) {
                context.view->overlay_style(node.id(), *culled_style);
            }
        }
        verdict = detail::cull_verdict(node, rect, painted_frame,
                                       culled_style.has_value() ? &*culled_style : nullptr);
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
            sink.supports_transform(), clip_scope, pass, scope.opacity);
        if (node.dirty() != Dirty::Clean) {
            pass.cache->note_miss();
        } else if (const PaintRecord* record = pass.cache->find(*signature, node.paint_data())) {
            const PaintCounts counts = record->counts;
            const bool pushed_clip = record->pushed_clip;
            const RoundedClip clip = record->clip;
            // The record carries the effective opacity its ops were folded with, so the child walk
            // composites at the same product without resolving this node's style again.
            child_scope.opacity = record->effective_opacity;
            replay_paint_ops(record->ops, sink);
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
            if (open_frame) {
                sink.pop_transform();
            }
            detail::add_paint_counts(stats, counts);
            return;
        }
    }

    style::ComputedStyle style =
        culled_style.has_value() ? std::move(*culled_style) : styles.compute(node);
    if (context.view != nullptr) {
        // Paint-only: the override is applied to this local copy, never to the scene or the
        // caller's style provider.
        context.view->overlay_style(node.id(), style);
    }
    ++stats.nodes;

    // On a miss the node's own emission is captured while it is forwarded to the real sink.
    std::optional<PaintCache::Recorder> recording;
    PaintSink* own = &sink;
    if (signature.has_value()) {
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
    const float effective_opacity = scope.opacity * detail::opacity_value(style);
    child_scope.opacity = effective_opacity;

    if (rect.width > 0.0f && rect.height > 0.0f) {
        const rhi::Color current = detail::current_color(style);
        // The backdrop sits beneath everything the node paints, including its box shadow.
        paint_backdrop(style, rect, effective_opacity, context, *own, stats);
        // The shadow sits beneath the node's own background, borders and content.
        const std::optional<float> shadow_radius = detail::uniform_radius(style);
        detail::paint_box_shadows(style, rect, shadow_radius.value_or(0.0f), effective_opacity,
                                  current, *own, stats, context.reduce_effects);
        paint_box_and_border(node, style, rect, effective_opacity, current, context.device_scale,
                             *own, stats);
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
    detail::paint_text_lines(node, style, rect, effective_opacity, *own, stats, context,
                             detail::lcd_text_eligible(context, effective_opacity,
                                                       painted_frame.active, interpolated,
                                                       painted_world));
    detail::paint_vector_content(node, style, rect, effective_opacity, *own, stats);
    detail::paint_image_content(node, style, rect, effective_opacity, *own, stats);

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
    if (open_frame) {
        sink.pop_transform();
    }
}

} // namespace

SceneStyleProvider::SceneStyleProvider(const style::CompiledStyleSheet& sheet,
                                       style::SceneStyleTree& tree)
    : sheet_(&sheet),
      tree_(&tree) {}

style::ComputedStyle SceneStyleProvider::compute(const Node& node) {
    return tree_->compute(*sheet_, node);
}

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
    pass.glass_max_blur_radius = context.glass.max_blur_radius;
    pass.glass_max_backdrop_area = context.glass.max_backdrop_area;
    pass.cull = context.cull.has_value() ? &*context.cull : nullptr;
    paint_node(root, styles, sink, stats, effective_context, Frame{}, PaintScope{},
               detail::ClipScope{}, pass);
    return stats;
}

} // namespace tessera::ui::paint
