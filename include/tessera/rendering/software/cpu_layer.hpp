// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"

namespace tessera::rendering::software {

// The CPU twin of the GPU's isolated layers and layer masks (issue #617, offscreen-layers.md
// section 5.2 and 5a). `CpuCanvas` is an opaque framebuffer, so a layer that must keep its own
// alpha is drawn twice, in the same order and with the same blend, onto two canvases of the frame's
// size: `black`, cleared to opaque black, and `white`, cleared to opaque white. A source-over of
// straight-alpha content gives, per channel,
//   black = P            (the premultiplied colour)
//   white = P + (1 - A)  (the colour plus what the layer let through)
// so `P = black` and `A = 1 - (white - black)` are recoverable exactly, up to the 8-bit rounding
// the canvas already applies. Both canvases stay consistent when a nested layer composites into
// them, because the composite below is linear in the backdrop.
//
// The alpha of a pixel is read as the rounded mean of the three channel differences, which absorbs
// the 1 LSB the per-channel rounding can introduce.

// Composites a layer into `destination` once, the way the GPU does: the layer's premultiplied
// colour scaled by `opacity` over the backdrop, which the layer's own alpha (times `opacity`)
// dims.
//   out = round(P * opacity + backdrop * (1 - opacity * A))
// Only pixels whose centre lies in `region` (the same rule as `CpuCanvas::fill_rect`) are touched.
// `destination`, `black` and `white` must have the same size, otherwise nothing happens.
void composite_layer(CpuCanvas& destination, const CpuCanvas& black, const CpuCanvas& white,
                     const rhi::Rect& region, float opacity);

// `mask-mode: alpha`: scales every channel and the alpha of the layer (`black`, `white`) by the
// alpha of the mask image (`mask_black`, `mask_white`, drawn the same two-canvas way), which is the
// GPU's DestinationIn step. Where the mask drew nothing its alpha is 0 and the layer is a hole.
// All four canvases must have the same size, otherwise nothing happens.
void apply_layer_mask(CpuCanvas& black, CpuCanvas& white, const CpuCanvas& mask_black,
                      const CpuCanvas& mask_white);

} // namespace tessera::rendering::software
