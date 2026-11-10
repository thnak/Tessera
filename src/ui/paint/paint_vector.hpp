// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint::detail {

// Emits the node's PaintData vector payload (PaintKind::Vector) after the node's background/borders
// and inside its active clip. Geometry and paint resolution live here; the public PaintSink hook is
// draw_vector. This is the vector counterpart of paint_text_lines in paint_text.cpp.
void paint_vector_content(const Node& node, const style::ComputedStyle& style,
                          const rhi::Rect& border_box, PaintSink& sink, PaintStats& stats);

} // namespace tessera::ui::paint::detail
