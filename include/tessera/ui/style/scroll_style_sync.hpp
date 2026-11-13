// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstddef>

namespace tessera::ui::style {

class CompiledStyleSheet;
class SceneStyleTree;

struct ScrollStyleSyncResult {
    std::size_t nodes = 0;   // nodes whose style was resolved
    std::size_t changed = 0; // nodes whose scroll runtime input changed
};

// Copies the computed scroll runtime inputs onto a scene node (docs/specs/stylesheet-v1.md
// section 11): `scroll-behavior`, `overscroll-behavior` and `overscroll-effect`. These are runtime
// policy inputs for the scroll controller, never layout inputs, so the setters mark no dirty
// domain. `scrollbar-gutter` is deliberately absent: it is a layout input read by
// `src/ui/layout/layout_style.cpp`, not scene state. Returns true when any value changed.
[[nodiscard]] bool apply_scroll_style(Node& node, const ComputedStyle& style);

// Walks `root` and applies the scroll runtime inputs of every node's computed style, resolving it
// through the same captured `SceneStyleTree` the paint sync uses. Call it between style resolution
// and the frame's layout/paint work; a steady scene stays clean because the setters are no-ops when
// the value is unchanged. No allocation beyond the styles the tree already caches.
[[nodiscard]] ScrollStyleSyncResult sync_scroll_style(Node& root, SceneStyleTree& tree,
                                                      const CompiledStyleSheet& sheet);

} // namespace tessera::ui::style
