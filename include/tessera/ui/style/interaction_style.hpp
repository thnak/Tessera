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
    std::size_t changed_visibility = 0;     // nodes whose `visibility` hidden state changed
    std::size_t changed_cursor = 0;         // nodes whose resolved cursor kind changed
};

// Copies the interaction properties the scene itself consumes from the computed style onto the
// nodes (stylesheet-v1 section 11, issues #213, #247 and #191):
//   pointer-events: auto|none -> Node::set_pointer_events (the hit test skips a `none` node but
//                                still searches its subtree, so a descendant with `auto`
//                                re-enables)
//   user-select: auto|none|text -> Node::set_user_select (Node::text_selection_allowed gates the
//                                selection gesture; editable text ignores it)
//   visibility: visible|hidden|collapse -> Node::set_style_hidden (paint, hit testing and the
//                                accessibility tree skip the subtree; the layout box stays, so
//                                unlike `display: none` the parent does not reflow)
//   cursor: the CSS cursor keywords -> Node::set_cursor (the app maps the hovered node's kind onto
//                                the HAL's CursorShape; the scene never sees an OS cursor)
// Invisible subtrees are skipped, like painting and hit testing. Setters are no-ops when the value
// is unchanged, so a steady scene stays clean. This is the interaction half of the
// computed-style -> scene seam; it runs next to `paint::sync_scene_style` (which owns z-index,
// transform, shadow reach and glass), after layout has written the bounds.
//
// All four properties are inherited, so storing each node's computed value is the ancestor
// resolution: a `none` parent with an explicit `auto` child computes `auto` on the child, exactly
// as CSS does. `visibility` is the exception in V1: a hidden subtree is not walked, so a descendant
// cannot re-enable itself (see the note on `Node::set_style_hidden`). `cursor` resolves an
// unrecognised keyword or an unsupported `url()` list to `auto`, so V1 never hands the platform a
// shape it did not implement.
InteractionStyleSyncResult sync_interaction_style(Node& root, InteractionStyleFn compute,
                                                  void* context);

} // namespace tessera::ui::style
