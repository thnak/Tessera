// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

#include <cstdint>

namespace tessera::ui::paint::detail {

struct ListNumber {
    bool item = false;
    std::int32_t value = 0;
};

// The counter of a list item: its 1-based position among the `role="listitem"` children of its
// parent, moved by the parent's `aria-start` (HTML `ol start`) and `aria-reversed` (`reversed`,
// which counts down from the item count when there is no start) and reset by an item's `aria-value`
// (`li value`), as HTML numbers an `ol`. `item` is false when the node is not a list item. It is
// part of the paint cache key: inserting a sibling or changing the list's start changes a marker
// without dirtying the item.
[[nodiscard]] ListNumber list_item_number(const Node& node);

// Paints the marker of a list item (docs/design/list-style.md): a disc, ring or square drawn as
// geometry, or the counter text of an ordered type, in the item's colour, outside its border box
// and aligned to the first line. Nothing for `list-style-type: none` or a node that is not a list
// item. `opacity` is the node's effective opacity.
void paint_list_marker(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                       float opacity, const PaintContext& context, PaintSink& sink,
                       PaintStats& stats);

} // namespace tessera::ui::paint::detail
