// SPDX-License-Identifier: MIT
//
// The identity of a retainable layer (issue #617, docs/design/gpu/offscreen-layers.md section 5.6).
// A retained layer is reused only when everything that decides its pixels is the same as when it
// was rendered. The key folds what the pass can see without resolving style: the node's whole
// PaintCacheSignature (geometry, sink-space mapping, clip stack, inherited opacity, context
// flags), the node's own opacity, its filter chain, whether it is masked, and the frame
// pipeline's retention epoch (a style or tree generation change, or a frame that painted without
// seeing a change). What the key cannot see - the node's computed style and its descendants -
// is covered by the dirty contract the paint cache already relies on: the sink is told whether the
// node and its subtree are clean, and a dirty node never matches.

#pragma once

#include "tessera/rendering/gpu/layer_filter.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <cstdint>

namespace tessera::ui::paint::detail {

// The paint-space box a retained layer covers: the node's subtree paint box (the superset the paint
// cull and the damage pass already rely on) placed at the node's origin `rect`. A layer the paint
// pass opens without retention keeps the frame-sized box and lets the renderer clamp it; a retained
// layer is stored at its own size, so a frame-sized box would cost a whole frame of memory per
// node. An unbounded subtree (text that may overflow) has no box and falls back to the whole frame.
[[nodiscard]] rhi::Rect retained_layer_box(const Node& node, const rhi::Rect& rect);

// Asks `sink` whether it retains the node's layer, then pushes the layer either way. True: the sink
// holds the pixels, so the caller pops the layer at once and paints nothing inside it.
[[nodiscard]] bool push_layer_retained(PaintSink& sink, const Node& node,
                                       const PaintCacheSignature& signature, const LayerDraw& layer,
                                       bool masked, std::uint64_t epoch);

[[nodiscard]] std::uint64_t layer_retention_key(const PaintCacheSignature& signature,
                                                float own_opacity,
                                                const rendering::gpu::LayerFilterChain& filter,
                                                bool masked, std::uint64_t epoch);

} // namespace tessera::ui::paint::detail
