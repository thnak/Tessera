// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/list_style.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>
#include <optional>

namespace tessera::ui::paint {

// `list-style-position: inside` (docs/design/list-style-inside.md). A list item is a View, so the
// first line it flows a marker into belongs to its first descendant Text: the item's marker is a
// first-line box of that Text. Layout and paint both resolve it here, so the box layout reserved
// is the box paint draws into.
struct InsideMarker {
    // The `role="listitem"` node whose marker it is.
    const Node* item = nullptr;
    // The item's computed style: the marker takes its colour and font, as an outside marker does.
    style::ComputedStyle item_style;
    style::ListStyleType type = style::ListStyleType::None;
    std::int32_t ordinal = 0;
    // The item's font, the one the marker is measured and drawn in.
    text::TextStyle text_style;
    // Width of the marker box in px: the room reserved at the start of the first line.
    float width = 0.0f;
};

// The first descendant `Text` of a list item in tree order, or nullptr. A nested list item is not
// entered: its text is its own marker's line, not the outer item's.
[[nodiscard]] const Node* first_item_text(const Node& item);

// The nearest `role="listitem"` ancestor of `text` when `text` is that item's first Text, else
// nullptr. Cheap; reads no style.
[[nodiscard]] const Node* marker_host_item(const Node& text);

// The inside marker `text` hosts: it is the first Text of a list item whose computed
// `list-style-position` is `inside` and whose `list-style-type` is not `none`. `measure` gives the
// advance of a counter's text (the same seam as the wrap: layout's measurer or paint's metrics
// source); bullets are font-relative and measure nothing.
[[nodiscard]] std::optional<InsideMarker> inside_marker(const Node& text, StyleProvider& styles,
                                                        text::TextAdvanceMeasure measure,
                                                        const void* user);

} // namespace tessera::ui::paint
