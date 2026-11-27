// SPDX-License-Identifier: MIT
//
// The glass backdrop and the box visuals of one node (split out of paint_pass.cpp).

#include "paint_box.hpp"
#include "paint_gradient.hpp"
#include "paint_node_parts.hpp"
#include "paint_shadow.hpp"

#include <algorithm>
#include <optional>

namespace tessera::ui::paint::detail {

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
    const std::optional<style::BackdropFilter> filter = backdrop_filter_value(style);
    if (!filter.has_value()) {
        return;
    }
    const float sigma = rendering::gpu::clamp_glass_sigma(filter->blur_sigma, context.glass);

    BackdropDraw draw;
    draw.rect = rect;
    draw.clip = corner_clip(style, rect);
    draw.sigma = sigma;
    draw.opacity = std::clamp(opacity, 0.0f, 1.0f);
    draw.saturate = filter->saturate;
    draw.brightness = filter->brightness;
    draw.contrast = filter->contrast;
    sink.draw_backdrop(draw);
    ++stats.backdrops;
}

// The node's own box visuals in paint order: background, gradient, border, then the inset shadows.
// CSS paints an inset shadow above the background and below the content (CSS Backgrounds 3 section
// 7.1.1), which is why the outer shadows are emitted before this call and the inset ones inside it.
void paint_box_and_border(const Node& node, const style::ComputedStyle& style,
                          const rhi::Rect& rect, float opacity, const rhi::Color& current,
                          float device_scale, PaintSink& sink, PaintStats& stats,
                          bool reduce_effects, BackgroundLayerCache* const layer_cache) {
    paint_background(style, rect, opacity, current, sink, stats);
    paint_gradient(node, style, rect, opacity, sink, stats, layer_cache);
    if (const std::optional<RingBorder> ring =
            rounded_ring(style, rect, opacity, current, device_scale)) {
        sink.fill_rounded_ring(rect, ring->color, ring->radius, ring->width);
        ++stats.borders;
    } else {
        paint_borders(style, rect, opacity, current, device_scale, sink, stats);
    }
    // The inset shadows sit above the background and border, below the content.
    paint_box_shadows(style, rect, uniform_radius(style).value_or(0.0f), opacity, current, sink,
                      stats, ShadowLayer::Inset, reduce_effects);
}

} // namespace tessera::ui::paint::detail
