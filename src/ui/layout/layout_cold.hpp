// SPDX-License-Identifier: MIT
//
// The from-scratch layout path: build a Yoga tree for one `LayoutInput` tree, lay it out, collect
// the boxes, free it. This is the reference implementation - `LayoutEngine::layout` uses the
// retained tree (layout_retained.cpp) instead, and this path stays for `layout_subtree`, for grid
// items and as the oracle the differential test compares the retained tree against.

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"

namespace tessera::ui::text {
class TextMeasureCache;
}

namespace tessera::ui::layout::detail {

// Lays out `root` as a whole tree. `as_grid_item` zeroes the root's margins because the grid engine
// positions the item's border box and folds the margins into the track sizes.
[[nodiscard]] LayoutResult layout_cold(const LayoutInput& root, const LayoutViewport& viewport,
                                       float available_width, float available_height,
                                       text::TextMeasureCache& cache, bool as_grid_item);

} // namespace tessera::ui::layout::detail
