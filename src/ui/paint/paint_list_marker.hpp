// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/list_marker_inside.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/style/list_style.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>

namespace tessera::ui::paint::detail {

inline constexpr const char* kListItemRole = "listitem";

// Geometry in em of the shape markers, measured against Chrome's default `disc` at 16px: a 5px
// shape whose right edge sits 0.55em before the item and whose centre is 0.35em above the baseline.
inline constexpr float kShapeEm = 0.3125f;
inline constexpr float kShapeGapEm = 0.55f;
inline constexpr float kShapeRiseEm = 0.35f;

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

// Draws one marker (shape or counter text) whose right edge is `anchor_right`, on the line that
// starts at `line_top` with its baseline at `baseline`. An outside marker anchors at the item's
// left edge; an inside marker anchors at the end of the first-line box it was given. Shared so both
// placements draw identical glyphs (docs/design/list-style-inside.md).
void draw_list_marker(style::ListStyleType type, std::int32_t ordinal,
                      const text::TextStyle& text_style, const PaintTextMetricsSource& source,
                      const rhi::Color& color, float anchor_right, float line_top, float baseline,
                      float line_height, PaintSink& sink, PaintStats& stats);

// Draws the inside marker of the Text it was resolved for into the first-line box the wrap
// reserved (`text_style.first_line_indent`, set from `marker.width`). `content` is the Text's
// content box; the first line's start, baseline and height come from the same layout the glyphs
// use, so the marker sits on that line whatever the text alignment.
void paint_inside_marker(const InsideMarker& marker, std::string_view text,
                         const text::TextStyle& text_style, const PaintTextMetricsSource& source,
                         bool wrap, const rhi::Rect& content, PaintTextAlign align, float opacity,
                         PaintSink& sink, PaintStats& stats);

// Paints the marker of a list item (docs/design/list-style.md): a disc, ring or square drawn as
// geometry, or the counter text of an ordered type, in the item's colour, outside its border box
// and aligned to the first line. Nothing for `list-style-type: none` or a node that is not a list
// item. `opacity` is the node's effective opacity.
void paint_list_marker(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                       float opacity, const PaintContext& context, PaintSink& sink,
                       PaintStats& stats);

} // namespace tessera::ui::paint::detail
