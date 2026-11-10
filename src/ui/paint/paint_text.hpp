// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint::detail {

// Emits the node's PaintData text payload (PaintKind::Text): glyph runs plus optional caret,
// selection and IME preedit decoration. Runs after the node's background/borders and inside its
// active clip; the vector/image counterparts are paint_vector_content/paint_image_content.
void paint_text_lines(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                      PaintSink& sink, PaintStats& stats, const PaintContext& context);

} // namespace tessera::ui::paint::detail
