// SPDX-License-Identifier: MIT

#include "paint_cache_internal.hpp"
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
#include <cmath>
#include <cstddef>
#include <optional>

namespace tessera::ui::paint {
namespace {

// A transformed node (CSS `transform`) is painted in its own local frame when the sink can apply a
// matrix: the sink holds `world` (the node's full world matrix) and every rect in the subtree is
// expressed relative to it. `active` is false for plain scenes, which keep painting world bounds.
struct Frame {
    bool active = false;
    rhi::Mat3 world;
    rhi::Mat3 inverse;
};

bool is_identity(const rhi::Mat3& matrix) {
    const rhi::Mat3 identity;
    for (int index = 0; index < 9; ++index) {
        if (matrix.m[index] != identity.m[index]) {
            return false;
        }
    }
    return true;
}

rhi::Rect transformed_aabb(const rhi::Mat3& matrix, const rhi::Rect& rect) {
    float xs[4];
    float ys[4];
    matrix.transform_point(rect.x, rect.y, xs[0], ys[0]);
    matrix.transform_point(rect.right(), rect.y, xs[1], ys[1]);
    matrix.transform_point(rect.right(), rect.bottom(), xs[2], ys[2]);
    matrix.transform_point(rect.x, rect.bottom(), xs[3], ys[3]);
    const auto [min_x, max_x] = std::minmax_element(xs, xs + 4);
    const auto [min_y, max_y] = std::minmax_element(ys, ys + 4);
    return rhi::Rect{*min_x, *min_y, *max_x - *min_x, *max_y - *min_y};
}

// Bounds are parent-relative; Node::world_bounds() composes them into absolute coordinates, so a
// plain pass paints that rect directly (see paint_pass.hpp). Inside a transformed frame the rect is
// the node's box relative to the frame: between a frame and its descendants only translations
// (and scroll offsets) remain, so this is exact.
//
// `painted_world` is the matrix the node paints with and `interpolated` says whether an
// interpolation view moved it away from Node::world_transform(). When it did not, the authoritative
// accessors are used verbatim so a non-interpolated frame is byte-identical to before the view
// existed.
rhi::Rect paint_rect(const Node& node, const Frame& frame, bool interpolated,
                     const rhi::Mat3& painted_world) {
    if (!frame.active) {
        if (!interpolated) {
            return node.world_bounds();
        }
        return transformed_aabb(painted_world,
                                rhi::Rect{0.0f, 0.0f, node.bounds().width, node.bounds().height});
    }
    const rhi::Mat3 relative = frame.inverse * painted_world;
    return transformed_aabb(relative,
                            rhi::Rect{0.0f, 0.0f, node.bounds().width, node.bounds().height});
}

// Clips are device-space state: inside a transformed frame a local clip is mapped through the
// frame. Scale/translate/quarter-turn frames map a rect to a rect; any other frame cannot be
// represented by a rounded rect, so its overflow clip is dropped rather than painted wrong.
std::optional<RoundedClip> device_clip(const RoundedClip& clip, const Frame& frame) {
    if (!frame.active) {
        return clip;
    }
    const rhi::Mat3& m = frame.world;
    constexpr float kEpsilon = 1.0e-4f;
    const bool axis_scale = std::abs(m.m[1]) < kEpsilon && std::abs(m.m[3]) < kEpsilon;
    const bool quarter_turn = std::abs(m.m[0]) < kEpsilon && std::abs(m.m[4]) < kEpsilon;
    if (!axis_scale && !quarter_turn) {
        return std::nullopt;
    }
    const float scale = std::sqrt(std::abs(m.m[0] * m.m[4] - m.m[1] * m.m[3]));
    RoundedClip mapped;
    mapped.rect = transformed_aabb(m, clip.rect);
    mapped.top_left = clip.top_left * scale;
    mapped.top_right = clip.top_right * scale;
    mapped.bottom_right = clip.bottom_right * scale;
    mapped.bottom_left = clip.bottom_left * scale;
    return mapped;
}

float clamp_radius(float radius, const rhi::Rect& rect) {
    return std::min({radius, rect.width * 0.5f, rect.height * 0.5f});
}

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
                   const rhi::Color& current, PaintSink& sink, PaintStats& stats) {
    const detail::BorderWidths widths = detail::border_widths(style, rect);
    const float top = widths.top;
    const float right = widths.right;
    const float bottom = widths.bottom;
    const float left = widths.left;

    const float middle_height = rect.height - top - bottom;

    emit_rect(sink, rhi::Rect{rect.x, rect.y, rect.width, top},
              detail::color_value(style, "border-top-color", current), opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x + rect.width - right, rect.y + top, right, middle_height},
              detail::color_value(style, "border-right-color", current), opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + rect.height - bottom, rect.width, bottom},
              detail::color_value(style, "border-bottom-color", current), opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + top, left, middle_height},
              detail::color_value(style, "border-left-color", current), opacity, stats.borders);
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
    draw.device_scale = context.device_scale > 0.0f && std::isfinite(context.device_scale)
                            ? context.device_scale
                            : 1.0f;
    sink.draw_backdrop(draw);
    ++stats.backdrops;
}

// A scroll container clips its children to its scrollport. When the scene owner set
// `scrollport_insets` (typically to the painted border widths), the clip uses them so clamping,
// painting, and hit testing agree; otherwise it falls back to the style-derived padding box, which
// keeps the default output byte-identical for borderless and bordered containers.
std::optional<rhi::Rect> scrollport_clip(const Node& node, const style::ComputedStyle& style,
                                         const rhi::Rect& rect) {
    if (!node.has_scrollport_insets()) {
        return detail::padding_box(style, rect);
    }
    const float width = rect.width - node.scrollport_left() - node.scrollport_right();
    const float height = rect.height - node.scrollport_top() - node.scrollport_bottom();
    if (width <= 0.0f || height <= 0.0f) {
        return std::nullopt;
    }
    return rhi::Rect{rect.x + node.scrollport_left(), rect.y + node.scrollport_top(), width,
                     height};
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

void paint_node(const Node& node, StyleProvider& styles, PaintSink& sink, PaintStats& stats,
                const PaintContext& context, Frame frame, const PaintScope& scope,
                const detail::ClipScope& clip_scope, const detail::PassState& pass) {
    if (!node.visible()) {
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

    // A node with its own transform opens a new local frame when the sink can apply the matrix. The
    // decision is taken before any emission so a cache hit can replay the recorded push instead of
    // emitting one twice.
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

    style::ComputedStyle style = styles.compute(node);
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
    // It multiplies every primitive this node emits and is exactly what its children inherit, so a
    // group fades as a whole without an offscreen layer (paint-pass.md section 14.1). `opacity: 0`
    // still walks the subtree: the scene and its hit testing are untouched, only the emits vanish.
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
        paint_background(style, rect, effective_opacity, current, *own, stats);
        paint_linear_gradient(node, style, rect, effective_opacity, *own, stats);
        paint_borders(style, rect, effective_opacity, current, *own, stats);
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
    // Text and vector runs are content: paint them after the element's own background/borders and
    // inside its own overflow/scroll clip, before the children (paint order). PaintKind selects one
    // active payload; the other is ignored.
    detail::paint_text_lines(node, style, rect, effective_opacity, *own, stats, context);
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
    pass.glass_max_blur_radius = context.glass.max_blur_radius;
    pass.glass_max_backdrop_area = context.glass.max_backdrop_area;
    paint_node(root, styles, sink, stats, context, Frame{}, PaintScope{}, detail::ClipScope{},
               pass);
    return stats;
}

} // namespace tessera::ui::paint
