// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstddef>

namespace tessera::ui::style {

// Computes one node's style for the sync below. The frame pipeline binds its
// `paint::StyleProvider`; tests pass a stub. Never null.
using InteractionStyleFn = ComputedStyle (*)(void* context, const Node& node);

struct InteractionStyleSyncResult {
    std::size_t nodes = 0;                  // visible nodes whose style was inspected
    std::size_t changed_pointer_events = 0; // nodes whose hit-test participation changed
    std::size_t changed_user_select = 0;    // nodes whose selection policy changed
};

// Copies the two interaction properties the scene itself consumes from the computed style onto the
// nodes (stylesheet-v1 section 11, issue #247):
//   pointer-events: auto|none -> Node::set_pointer_events (the hit test skips a `none` node but
//                                still searches its subtree, so a descendant with `auto`
//                                re-enables)
//   user-select: auto|none|text -> Node::set_user_select (Node::text_selection_allowed gates the
//                                selection gesture; editable text ignores it)
// Invisible subtrees are skipped, like painting and hit testing. Setters are no-ops when the value
// is unchanged, so a steady scene stays clean. This is the interaction half of the
// computed-style -> scene seam; it runs next to `paint::sync_scene_style` (which owns z-index,
// transform, shadow reach and glass), after layout has written the bounds.
//
// Both properties are inherited, so storing each node's computed value is the ancestor resolution:
// a `none` parent with an explicit `auto` child computes `auto` on the child, exactly as CSS does.
InteractionStyleSyncResult sync_interaction_style(Node& root, InteractionStyleFn compute,
                                                  void* context);

} // namespace tessera::ui::style
