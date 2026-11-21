// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/selector.hpp"

namespace tessera::ui::style {

// Whether a node's match depends only on the node itself and its ancestors. A sibling combinator,
// `:has()` and the structural pseudo-classes read other nodes, so a change to one node can restyle
// a node outside its subtree; a sheet with none of them can restyle just the changed subtrees
// (issue #711).
[[nodiscard]] bool selector_is_subtree_local(const ComplexSelector& selector);
[[nodiscard]] bool selector_list_is_subtree_local(const SelectorList& list);

} // namespace tessera::ui::style
