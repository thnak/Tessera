// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <cstddef>
#include <vector>

namespace tessera::ui::layout {

struct LayoutApplyResult {
    std::size_t applied = 0;
    std::size_t missing = 0;
    std::size_t extra = 0;
};

LayoutApplyResult apply_layout(const LayoutResult& result, Node& root);

// Same, and appends to `resized` (cleared first) the topmost nodes whose box WIDTH or HEIGHT
// changed. Position alone is not a size change. "Topmost" means no ancestor is listed: the scoped
// style sync walks each listed node's whole subtree, so listing a descendant too would visit it
// twice and a full-window resize would otherwise cost nodes x depth. The size is what the style
// sync reads from the box (a percentage transform origin resolves against it), so a node outside
// this list and outside the restyled roots has nothing to re-apply. The vector keeps its capacity
// between frames.
LayoutApplyResult apply_layout(const LayoutResult& result, Node& root,
                               std::vector<const Node*>& resized);

} // namespace tessera::ui::layout
