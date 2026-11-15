// SPDX-License-Identifier: MIT
//
// The paint half of the hybrid chart node (issue #183): the retained emission of
// ChartSeriesGeometry plus the immediate-mode escape hatch. Normative contract:
// docs/specs/chart-node-v1.md.
//
// Split from chart_node.hpp because the two halves have different dependencies: geometry, damage
// and sampling need only the rendering types, while emission needs ui::paint. A caller that only
// feeds samples (a data producer, a test of the damage rule) never compiles the paint stack.
//
// Damage clipping is NOT reimplemented here. paint_chart emits every retained box into whatever
// sink it is given, so a partial redraw wraps it in paint::ClippedPaintSink - the same sink
// FramePipeline uses for every other paint content - and boxes outside the plan's regions emit
// nothing. That is what keeps a chart from owning a second damage mechanism: ChartDamage feeds
// rendering::DamageAccumulator, the accumulator feeds make_redraw_plan, and the plan feeds the
// sink.
//
// Emission is one fill_rect per retained box. That is deliberate for V1: a chart's repaint cost is
// dominated by how many boxes survive the damage filter, not by the batching of the boxes that do,
// and Batched2DRenderer has no polyline primitive to emit instead. When a stroked primitive lands
// in the renderer, this is the one function that changes.

#pragma once

#include "tessera/ui/chart/chart_node.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <algorithm>
#include <cstddef>

namespace tessera::ui::chart {

// Folds `opacity` into `color`. False when the resulting alpha is zero, i.e. the series would emit
// nothing at all - which is what keeps a fully faded chart at zero sink calls.
[[nodiscard]] inline bool chart_apply_opacity(rhi::Color& color, float opacity) noexcept {
    const float alpha = std::clamp(color.a, 0.0f, 1.0f) * std::clamp(opacity, 0.0f, 1.0f);
    if (!(alpha > 0.0f)) {
        return false;
    }
    color.a = alpha;
    return true;
}

// Retained pass: every retained segment, then every retained marker, in series order. Boxes outside
// the sink's clip (or outside a damage plan's regions) are dropped by the sink, not here.
inline void paint_chart(const ChartNode& chart, paint::PaintSink& sink, float opacity = 1.0f) {
    for (std::size_t index = 0; index < chart.series_count(); ++index) {
        rhi::Color color = chart.style(index).color;
        if (!chart_apply_opacity(color, opacity)) {
            continue;
        }
        const ChartSeriesGeometry& geometry = chart.geometry(index);
        for (const rhi::Rect& box : geometry.segments) {
            if (!empty_rect(box)) {
                sink.fill_rect(box, color);
            }
        }
        for (const rhi::Rect& box : geometry.markers) {
            if (!empty_rect(box)) {
                sink.fill_rect(box, color);
            }
        }
    }
}

// Runs the immediate-mode hook, if one is installed. Called AFTER the retained pass so an
// annotation draws over the plot.
inline void paint_chart_immediate(const ChartNode& chart, ChartImmediateFn draw, void* context,
                                  paint::PaintSink& sink, float opacity = 1.0f) {
    if (draw == nullptr) {
        return;
    }
    const ChartImmediateDraw request{&sink, &chart.plot(), opacity};
    draw(context, request);
}

// The whole chart: retained geometry, then the immediate hook.
inline void paint_chart(ChartNode& chart, paint::PaintSink& sink, float opacity,
                        ChartImmediateFn draw, void* context) {
    paint_chart(static_cast<const ChartNode&>(chart), sink, opacity);
    paint_chart_immediate(chart, draw, context, sink, opacity);
}

} // namespace tessera::ui::chart
