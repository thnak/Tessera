// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

#include <cstdint>

namespace tessera::ui::paint::detail {

// The 1-based position of a list item among the `role="listitem"` children of its parent, or 0 when
// the node is not a list item. It is the counter an ordered marker shows and part of the paint
// cache key: inserting a sibling before an item changes its marker without dirtying the item.
[[nodiscard]] std::uint32_t list_item_ordinal(const Node& node);

// Paints the marker of a list item (docs/design/list-style.md): a disc, ring or square drawn as
// geometry, or the counter text of an ordered type, in the item's colour, outside its border box
// and aligned to the first line. Nothing for `list-style-type: none` or a node that is not a list
// item. `opacity` is the node's effective opacity.
void paint_list_marker(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                       float opacity, const PaintContext& context, PaintSink& sink,
                       PaintStats& stats);

} // namespace tessera::ui::paint::detail
