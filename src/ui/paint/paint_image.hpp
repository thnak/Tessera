// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint::detail {

// Emits the node's PaintData raster-image payload (PaintKind::Image) after the node's
// background/borders and inside its active clip. Geometry/opacity resolution lives in
// paint_content.hpp; all decode/upload work is the sink's job (draw_image). This is the image
// counterpart of paint_vector_content.
void paint_image_content(const Node& node, const style::ComputedStyle& style,
                         const rhi::Rect& border_box, PaintSink& sink, PaintStats& stats);

} // namespace tessera::ui::paint::detail
