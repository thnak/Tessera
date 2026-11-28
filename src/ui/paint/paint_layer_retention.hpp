// SPDX-License-Identifier: MIT
//
// The identity of a retainable layer (issue #617, docs/design/gpu/offscreen-layers-retention.md).
// A retained layer is reused only when everything that decides its pixels is the same as when it
// was rendered. The key folds what the pass can see without resolving style: the node's whole
// PaintCacheSignature (geometry, sink-space mapping, clip stack, inherited opacity, context
// flags), the node's own opacity, its filter chain, whether it is masked, and the frame
// pipeline's retention epoch (a style or tree generation change, or a frame that painted without
// seeing a change). What the key cannot see - the node's computed style and its descendants -
// is covered by the dirty contract the paint cache already relies on: the sink is told whether the
// node and its subtree are clean, and a dirty node never matches.

#pragma once

#include "paint_cache_internal.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
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

// Generic promotion (issue #967). `promote_now`: the policy has seen this clean container qualify,
// so the pass sends it down the retained-layer path instead of replaying a flat record. A layer
// carries no ancestor fade (a flat paint folds it into every primitive and rounds differently) and
// no LCD text, and is never nested in another layer.
[[nodiscard]] bool promote_now(const PassState& pass, const Node& node, const rhi::Rect& rect,
                               float inherited_opacity, bool in_layer);

// After the node painted: reports what its subtree emitted so the policy can decide the next
// frame. `held` is `promote_now` as it was before the paint; a held node whose layer was
// composited from retained pixels painted nothing, so it is not observed (its history stands).
// A promoted node that turned dirty has its retained layer released through `sink` at once.
void observe_promotion(const PassState& pass, PaintSink& sink, const Node& node,
                       float inherited_opacity, bool in_layer, bool held, const PaintStats& before,
                       const PaintStats& after);

// The epoch folded into the retained-layer key of `node`; a promoted node's moves when it turns
// dirty while painted flat, so a layer recorded before that never matches the next promotion.
[[nodiscard]] std::uint64_t layer_key_epoch(const PassState& pass, const Node& node, bool promoted);

[[nodiscard]] std::uint64_t layer_retention_key(const PaintCacheSignature& signature,
                                                float own_opacity,
                                                const rendering::gpu::LayerFilterChain& filter,
                                                bool masked, std::uint64_t epoch);

} // namespace tessera::ui::paint::detail
