// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/text/hit_test.hpp"

#include <cstddef>
#include <optional>

namespace tessera::ui::paint {

// Where the caret of an editable text node (`TextInput`, `TextArea`) sits, and which byte a point
// lands on. They resolve the content box, wrap decision, vertical centring and line alignment the
// way `paint_text_lines` does, so the caret box is the rectangle paint draws the caret at and a
// hit is the byte the user sees under the pointer. `rect` is the node's border box in the
// coordinates the paint pass used (the canvas passes `world_bounds()`); `source` null means the
// deterministic stub, which is what a paint without a font uses.
//
// Both return nothing when the node carries no text payload or has no room for text.

// `byte_offset` is clamped to the text. The box is in `rect`'s coordinates: `x`/`y` the caret's
// top-left, `width` its painted thickness at `device_scale`, `height` its line's height.
[[nodiscard]] std::optional<CaretBox>
text_field_caret_box(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                     std::size_t byte_offset, const PaintTextMetricsSource* source,
                     float device_scale);

// The caret position (grapheme-cluster boundary) nearest to `(x, y)`, in `rect`'s coordinates. A
// point above or below the text snaps to the first or last line, one left or right of it to that
// line's ends, which is what a drag that leaves the field needs.
[[nodiscard]] std::optional<text::TextHit> text_field_hit(const Node& node,
                                                          const style::ComputedStyle& style,
                                                          const rhi::Rect& rect, float x, float y,
                                                          const PaintTextMetricsSource* source);

} // namespace tessera::ui::paint
