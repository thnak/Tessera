// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/layer_filter.hpp"
#include "tessera/rendering/gpu/mask_composite.hpp"
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

// `mask-composite`: combines one mask layer (`part_black`, `part_white`, the same two-canvas pair)
// into the mask so far (`mask_black`, `mask_white`) with `op`, on alpha only, exactly as the GPU's
// SourceOut / DestinationIn / Xor blend steps do (src = the layer, dst = the mask so far). The mask
// pair is rewritten with zero colour and the combined alpha, since a mask's colour is never read.
// All four canvases must have the same size, otherwise nothing happens.
void combine_mask_part(CpuCanvas& mask_black, CpuCanvas& mask_white, const CpuCanvas& part_black,
                       const CpuCanvas& part_white, gpu::MaskCompositeOp op);
// `filter` colour functions (brightness() ... opacity(), issue #617): applies the functions of `op`
// (a Kind::ColorFunctions op, see gpu/color_filter.hpp) in order to the layer's straight-alpha
// colour, which is recovered from the pair (P = black, A = 1 - (white - black)), then writes the
// result back as the pair of the new premultiplied colour and alpha. Like the GPU pass it runs
// before the mask and the opacity, and a pixel the layer never drew stays untouched. The canvases
// must have the same size, otherwise nothing happens.
void apply_layer_color_filter(CpuCanvas& black, CpuCanvas& white, const gpu::LayerFilterOp& op);

} // namespace tessera::rendering::software
