// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/background_layer_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint::detail {

// Gradient background: emitted after the solid background and before borders/content, at the node
// box clipped by the node's own border radius. The gradient is the node's PaintData payload when it
// has one, otherwise the stylesheet's `background-image` (issues #457 and #613). Stops are clamped
// to [0,1] and stably sorted; equal offsets keep input order (the later stop wins at the duplicate
// point). Node opacity is folded into each stop's alpha here, so sinks never see the node opacity
// separately.
//
// One entry point for both shapes: the resolved value decides whether a `GradientDraw` or a
// `RadialGradientDraw` reaches the sink, so the paint pass has one call site and the two shapes
// cannot drift in their normalization, opacity fold or emission order.
//
// `cache` memoises the `background-image` parse per style (issue #733 item 2); null parses the
// authored value on every call, which is the pre-cache behaviour and byte-identical output.
void paint_gradient(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                    float opacity, PaintSink& sink, PaintStats& stats,
                    BackgroundLayerCache* cache = nullptr);

// `mask-image` (issue #617): true when the style paints a mask at all (a layer list with at least
// one gradient; `none` and anything outside the `background-image` grammar do not).
[[nodiscard]] bool has_mask_image(const style::ComputedStyle& style);

// Emits the mask image of `rect` into `sink` as gradient fills whose alpha is the mask, between
// the sink's begin_layer_mask() and end_layer_mask(). Same grammar and draws as `background-image`
// (layers bottom to top), at the border box and unrounded, with no node opacity (the layer carries
// it). The authored value is parsed per call: a masked node is painted uncached, so a parse cache
// would only matter for a style shared by many masked nodes.
void paint_mask_image(const style::ComputedStyle& style, const rhi::Rect& rect, PaintSink& sink,
                      PaintStats& stats);

} // namespace tessera::ui::paint::detail
