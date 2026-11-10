// SPDX-License-Identifier: MIT

#include "paint_image.hpp"
#include "paint_properties.hpp"
#include "paint_shadow.hpp"
#include "paint_text.hpp"
#include "paint_vector.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
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
// (scroll offset applied) and whether any ancestor already paints with an interpolation override.
// `interpolated == false` keeps the authoritative Node::world_transform() path untouched.
struct PaintScope {
    rhi::Mat3 content_world;
    bool interpolated = false;
};

void paint_node(const Node& node, StyleProvider& styles, PaintSink& sink, PaintStats& stats,
                const PaintContext& context, Frame frame, const PaintScope& scope) {
    if (!node.visible()) {
        ++stats.hidden_subtrees;
        return;
    }

    style::ComputedStyle style = styles.compute(node);
    if (context.view != nullptr) {
        // Paint-only: the override is applied to this local copy, never to the scene or the
        // caller's style provider.
        context.view->overlay_style(node.id(), style);
    }
    ++stats.nodes;

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

    // A node with its own transform opens a new local frame when the sink can apply the matrix.
    bool pushed_transform = false;
    if (!is_identity(local) && sink.supports_transform()) {
        Frame opened;
        if (painted_world.try_inverse(opened.inverse)) {
            opened.active = true;
            opened.world = painted_world;
            sink.push_transform(frame.active ? frame.inverse * painted_world : painted_world);
            ++stats.transforms;
            pushed_transform = true;
            frame = opened;
        }
    }

    const rhi::Rect rect = paint_rect(node, frame, interpolated, painted_world);
    if (rect.width > 0.0f && rect.height > 0.0f) {
        const float opacity = detail::opacity_value(style);
        const rhi::Color current = detail::current_color(style);
        // The shadow sits beneath the node's own background, borders and content.
        const std::optional<float> shadow_radius = detail::uniform_radius(style);
        detail::paint_box_shadows(style, rect, shadow_radius.value_or(0.0f), opacity, current, sink,
                                  stats);
        paint_background(style, rect, opacity, current, sink, stats);
        paint_linear_gradient(node, style, rect, opacity, sink, stats);
        paint_borders(style, rect, opacity, current, sink, stats);
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
    if (clip_children) {
        const std::optional<rhi::Rect> clip_rect =
            scroll_clips ? scrollport_clip(node, style, rect) : detail::padding_box(style, rect);
        if (clip_rect) {
            if (const auto clip = device_clip(detail::corner_clip(style, *clip_rect), frame)) {
                sink.push_clip(*clip);
                ++stats.clips;
                pushed_clip = true;
            }
        }
    }
    // Text and vector runs are content: paint them after the element's own background/borders and
    // inside its own overflow/scroll clip, before the children (paint order). PaintKind selects one
    // active payload; the other is ignored.
    detail::paint_text_lines(node, style, rect, sink, stats, context);
    detail::paint_vector_content(node, style, rect, sink, stats);
    detail::paint_image_content(node, style, rect, sink, stats);
    PaintScope child_scope;
    child_scope.interpolated = interpolated;
    if (interpolated) {
        child_scope.content_world =
            painted_world * rhi::Mat3::translation(-node.scroll_x(), -node.scroll_y());
    }
    for_each_by_z(node.children(), false, [&](const Node& child) {
        paint_node(child, styles, sink, stats, context, frame, child_scope);
        return false;
    });
    if (pushed_clip) {
        sink.pop_clip();
    }
    if (pushed_transform) {
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
    paint_node(root, styles, sink, stats, context, Frame{}, PaintScope{});
    return stats;
}

} // namespace tessera::ui::paint
