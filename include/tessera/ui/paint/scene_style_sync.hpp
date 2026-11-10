// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>

namespace tessera::ui::paint {

struct SceneStyleSyncResult {
    std::size_t nodes = 0;           // visible nodes whose style was resolved
    std::size_t changed_z_index = 0; // nodes whose z-index changed
    std::size_t changed_transform = 0;
    std::size_t changed_outsets = 0; // nodes whose box-shadow reach changed
};

// Copies the computed style properties that the scene graph itself consumes onto the nodes:
//   z-index          -> Node::set_z_index   (`auto`/invalid = 0)
//   transform        -> Node::set_transform (around `transform-origin`, resolved against the node's
//                       current border box, so run this after layout has written the bounds)
//   box-shadow       -> Node::set_paint_outsets (the reach damage must cover)
// Invisible subtrees are skipped, like painting. Each setter only dirties paint/render when the
// value actually changed, so a steady scene stays clean. Call it between layout and painting
// whenever style or layout changed.
//
// Transform fidelity: hit testing and node boxes follow the full matrix. Painting draws each
// node's axis-aligned bounding box, so translate, scale and 90-degree rotations are exact for
// boxes; glyph sizes, border widths and corner radii do not scale, and other rotations/skews paint
// the bounding box of the transformed box rather than the rotated shape.
SceneStyleSyncResult sync_scene_style(Node& root, StyleProvider& styles);

} // namespace tessera::ui::paint
