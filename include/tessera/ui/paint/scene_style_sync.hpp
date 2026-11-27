// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node_fwd.hpp"
#include "tessera/ui/style/sync_memo.hpp"

#include <cstddef>
#include <span>

namespace tessera::ui::paint {

struct SceneStyleSyncResult {
    std::size_t nodes = 0;           // visible nodes whose style was resolved
    std::size_t changed_z_index = 0; // nodes whose z-index changed
    std::size_t changed_transform = 0;
    std::size_t changed_outsets = 0; // nodes whose box-shadow reach changed
    std::size_t glass_nodes = 0;     // visible nodes with a non-`none` backdrop-filter
    std::size_t changed_glass = 0;   // nodes whose resolved glass sigma changed
    std::size_t filter_nodes = 0;    // visible nodes with a layer `filter`
    std::size_t changed_filter = 0;  // nodes whose filter reach changed
};

// Copies the computed style properties that the scene graph itself consumes onto the nodes:
//   z-index          -> Node::set_z_index   (`auto`/invalid = 0)
//   transform        -> Node::set_transform (around `transform-origin`, resolved against the node's
//                       current border box, so run this after layout has written the bounds)
//   box-shadow       -> Node::set_paint_outsets (the reach damage must cover)
//   backdrop-filter  -> Node::set_glass_blur_sigma (sigma clamped by `glass`, the 3 sigma damage
//                       dependency of glass-v1 section 7) and Node::set_glass_subtree
//   filter           -> Node::set_filter_reach (the chain's halo, its own damage dependency, apart
//                       from the backdrop sigma) and Node::set_filter_subtree; the halo also folds
//                       into the paint outsets
// Invisible subtrees are skipped, like painting. Each setter only dirties paint/render when the
// value actually changed, so a steady scene stays clean. Call it between layout and painting
// whenever style or layout changed.
//
// Transform fidelity: hit testing and node boxes follow the full matrix. A sink that reports
// PaintSink::supports_transform() (Batched2DSink, and ClippedPaintSink in front of one) paints each
// transformed node and its subtree under the real matrix, so rotation, skew, scale, glyphs, borders
// and radii all transform. Overflow clips inside such a subtree map through scale/translate/
// quarter-turn frames and are dropped under other rotations/skews. Sinks without matrix support
// (the CPU sink) paint the node's axis-aligned bounding box instead: exact for translate, scale and
// quarter turns of boxes, an approximation otherwise.
SceneStyleSyncResult
sync_scene_style(Node& root, StyleProvider& styles,
                 const rendering::gpu::GlassLimits& glass = rendering::gpu::GlassLimits{},
                 style::SyncMemo* memo = nullptr);
// With a `memo` the walk skips a node whose style and box are unchanged since the last call (the
// glass limits must stay the same between calls that share one memo).

// Scoped variant (issue #735, slice B): visits only `roots` and their subtrees, plus the ancestors
// of each root (which refold `glass_subtree` and `filter_subtree`). A node outside every root kept
// its computed style, so re-deriving what it already applied is pure waste - that walk is ~25% of a
// class-change frame at 6k nodes.
//
// Sound only when the caller can prove every node outside `roots` kept its style AND its box. The
// layout-unchanged gate is exactly that proof: it declines unless the restyle changed no layout
// input, so no box moved. A caller that cannot prove it must use the unscoped overload.
//
// `roots` must be the topmost restyled nodes (a root's ancestors are visited for the glass refold,
// not for their own style). An empty `roots` visits nothing and returns a zero result, which is the
// right answer for a restyle that touched no node. The scene root is not a parameter: every node
// this walk needs is reachable from `roots` and their ancestors.
SceneStyleSyncResult
sync_scene_style_scoped(StyleProvider& styles, std::span<const Node* const> roots,
                        const rendering::gpu::GlassLimits& glass = rendering::gpu::GlassLimits{},
                        style::SyncMemo* memo = nullptr);

} // namespace tessera::ui::paint
