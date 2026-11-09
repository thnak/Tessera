// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/pixel_snap.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

namespace tessera::ui::layout {
namespace {

constexpr Dirty kDamageDomain = Dirty::Paint | Dirty::Render;
// Glyph ink can overhang the line box by ~1 px, the caret is up to 2 px at device scale >= 2, and
// the damage layer does not know the computed padding/top offsets: keep 4 px of slack around the
// line union. This is the documented ink/padding margin.
constexpr float kTextDamageMargin = 4.0f;

} // namespace

rendering::rhi::Rect node_damage_rect(const Node& node) {
    const rendering::rhi::Rect bounds = node.world_bounds();
    const auto& data = node.paint_data();
    if (!data || data->kind != PaintKind::Text || data->text.empty() ||
        data->text_style.font_size <= 0.0f) {
        return bounds;
    }
    const PaintTextAlign align = data->text_align.value_or(PaintTextAlign::Start);
    if (align != PaintTextAlign::Start) {
        return bounds; // alignment shifts lines within the box; keep the conservative box
    }
    if (data->decoration) {
        return bounds; // caret/selection can sit at either line edge; box is conservative
    }

    text::TextStyle style;
    style.font_size = data->text_style.font_size;
    if (data->text_style.font_weight > 0) {
        style.font_weight = data->text_style.font_weight;
    }
    style.nowrap = data->wrap != PaintWrap::Wrap;
    text::TextConstraints constraints;
    constraints.wrap = data->wrap == PaintWrap::Wrap;
    constraints.max_width = bounds.width;
    const text::FontMetrics metrics = text::StubFontMetricsProvider{}.metrics(style);
    text::TextLayout layout = text::layout_text(data->text, style, metrics, constraints);
    if (data->max_lines > 0 && layout.lines.size() > data->max_lines) {
        layout.lines.resize(data->max_lines);
    }
    if (layout.lines.empty()) {
        return bounds;
    }
    float height = 0.0f;
    for (const text::TextLine& line : layout.lines) {
        height += line.height;
    }
    const float top = bounds.y;
    return {bounds.x, top, bounds.width,
            std::min(bounds.height, height + 2.0f * kTextDamageMargin)};
}

DamagePassResult add_scene_damage(const DirtyPass& dirty, rendering::DamageAccumulator& damage,
                                  float dpi_scale, float margin) {
    DamagePassResult result;
    result.overflowed = dirty.overflowed();

    for (const DirtyNode& entry : dirty.nodes()) {
        if (entry.node == nullptr || !has_any(entry.flags, kDamageDomain)) {
            continue;
        }

        const rendering::rhi::Rect bounds = snap_rect(node_damage_rect(*entry.node), dpi_scale);
        if (bounds.width <= 0.0f || bounds.height <= 0.0f) {
            continue;
        }

        damage.add(bounds, margin);
        ++result.nodes;
    }

    // A truncated dirty list cannot prove the damage is complete: the missing nodes may have
    // changed pixels anywhere in their (unknown) bounds. Over-damage is cheap and safe;
    // under-damage leaves stale pixels, so an overflow forces a full-target redraw.
    if (dirty.overflowed()) {
        damage.force_full_redraw();
    }
    result.rects = damage.rects().size();
    result.full_redraw = damage.full_redraw();
    return result;
}

} // namespace tessera::ui::layout
