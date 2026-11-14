// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"

namespace tessera::ui::paint::detail {

// Emits the node's PaintData text payload (PaintKind::Text): glyph runs plus optional caret,
// selection and IME preedit decoration. Runs after the node's background/borders and inside its
// active clip; `opacity` is the node's effective opacity (its own x every ancestor's), so a text
// child fades with the group instead of only with its own value. The vector/image counterparts are
// paint_vector_content/paint_image_content.
void paint_text_lines(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                      float opacity, PaintSink& sink, PaintStats& stats,
                      const PaintContext& context);

// Paints a Text node that carries inline spans (issue #229): one paragraph measured with the
// parent's style, whose painted runs are split at span boundaries and carry each span's resolved
// weight/italic/colour. Also stores each span's run rectangle on its child node (in the parent's
// border-box space) so the generic hit test resolves a point to the span; the parent's box is the
// only layout box. `content` is the content box in absolute paint coordinates, `align` the resolved
// text-align, and `max_lines` truncates whole lines (0 = unlimited).
void paint_spanned_text(const Node& node, const text::TextStyle& text_style,
                        const rhi::Rect& content, const rhi::Color& color, float opacity, bool wrap,
                        PaintTextAlign align, std::uint32_t max_lines, const PaintContext& context,
                        PaintSink& sink, PaintStats& stats);

} // namespace tessera::ui::paint::detail
