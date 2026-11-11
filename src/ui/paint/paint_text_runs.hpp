// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <string_view>

// Glyph-run emission for one laid-out text line: the paint-time segment transform
// (docs/design/text.md section 6.2), the bidi visual-run order, and `text-overflow: ellipsis`
// (section 6.3). Split out of `paint_text.cpp`, which owns the payload/decoration orchestration, so
// each file stays within the source-size limit.
namespace tessera::ui::paint::detail {

// Visual x of a logical byte offset relative to the line start. Uses the bidi run order/affinity
// when the line has visual runs; pure LTR lines sum the painted segments of the prefix, so a
// collapsed interior run costs the one space advance the layout measured.
[[nodiscard]] float line_offset_x(std::string_view text, const text::TextLine& line,
                                  std::size_t offset, const text::TextStyle& style,
                                  const PaintTextMetricsSource& source);

// Emits one line's glyph runs left-to-right in visual order, applying the mode's paint-time segment
// transform and, when the style asks for it, `text-overflow: ellipsis`. `available_width` is the
// content-box width left from `line_x`; a line wider than it is truncated here, after layout, so no
// line box, width, or baseline changes.
void emit_line_runs(const text::TextLine& line, std::string_view text,
                    const text::TextStyle& text_style, const PaintTextMetricsSource& source,
                    float line_x, float line_top, float available_width, const rhi::Color& color,
                    PaintSink& sink, PaintStats& stats);

} // namespace tessera::ui::paint::detail
