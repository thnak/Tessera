// SPDX-License-Identifier: MIT

#pragma once

#include "paint_properties.hpp"
#include "paint_shadow.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"

namespace tessera::ui::paint::detail {

// The `filter` of a node as the layer chain the sink runs (issue #617): sigmas clamped to the
// configured `glassMaxBlurRadius` (the same cost bound the backdrop blur has), `currentcolor` and
// the omitted shadow colour resolved against `current_color`. Empty for `none`.
[[nodiscard]] rendering::gpu::LayerFilterChain
layer_filter_chain(const style::ComputedStyle& style, const rhi::Color& current_color,
                   const rendering::gpu::GlassLimits& limits);

// How far the filter's pixels reach past the node's border box (a blur grows every side by 3
// sigma, a drop-shadow by 3 sigma around its offset), so damage and culling cover the halo. The
// sums are conservative: each function adds its own reach to the previous one's.
[[nodiscard]] ShadowOutsets filter_outsets(const style::ComputedStyle& style,
                                           const rendering::gpu::GlassLimits& limits);

} // namespace tessera::ui::paint::detail
