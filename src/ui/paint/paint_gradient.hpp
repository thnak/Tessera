// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint::detail {

// Linear-gradient background: emitted after the solid background and before borders/content, at
// the node box clipped by the node's own border radius. The gradient is the node's PaintData
// payload when it has one, otherwise the stylesheet's `background-image` (issue #457). Stops are
// clamped to [0,1] and stably sorted; equal offsets keep input order (the later stop wins at the
// duplicate point). Node opacity is folded into each stop's alpha here, so sinks never see the node
// opacity separately.
void paint_linear_gradient(const Node& node, const style::ComputedStyle& style,
                           const rhi::Rect& rect, float opacity, PaintSink& sink,
                           PaintStats& stats);

} // namespace tessera::ui::paint::detail
