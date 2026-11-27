// SPDX-License-Identifier: MIT

#pragma once

#include "paint_properties.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint::detail {

// The two pieces of a node's own emission that need no traversal state (split out of
// paint_pass.cpp, which keeps the recursion). Both are emitted by `paint_node` in paint order: the
// glass backdrop first, then the box visuals.
void paint_backdrop(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                    const PaintContext& context, PaintSink& sink, PaintStats& stats);

void paint_box_and_border(const Node& node, const style::ComputedStyle& style,
                          const rhi::Rect& rect, float opacity, const rhi::Color& current,
                          float device_scale, PaintSink& sink, PaintStats& stats,
                          bool reduce_effects, BackgroundLayerCache* layer_cache);

} // namespace tessera::ui::paint::detail
