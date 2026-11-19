// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>

// `text-decoration` painting (docs/design/text-decoration.md): the underline, overline and
// line-through rules are plain rects emitted through the sink after the glyph runs they decorate.
// Not installed.
namespace tessera::ui::paint::detail {

namespace rhi = tessera::rendering::rhi;

struct TextRuleStyle {
    std::uint8_t lines = 0; // `style::kDecoration*` bits; 0 draws nothing
    rhi::Color color;       // resolved, node opacity already applied
    float thickness = 0.0f; // px; 0 derives it from the font size
};

// Resolves the rule style of one run: the lines and thickness the `TextStyle` carries, and the
// `text-decoration-color` of `computed` (`currentColor` -> `text_color`, which already carries
// `opacity`; an explicit colour has `opacity` applied here).
[[nodiscard]] TextRuleStyle resolve_text_rules(const style::ComputedStyle& computed,
                                               const text::TextStyle& text_style,
                                               const rhi::Color& text_color, float opacity);

// Draws the rules of `rules` across [left, right) for a run whose baseline is at `baseline_y`
// (absolute paint coordinates). Edges are rounded to whole pixels so rules of adjacent runs abut
// without a seam. Positions derive from `metrics` and `font_size`.
void emit_text_rules(const TextRuleStyle& rules, const text::FontMetrics& metrics, float font_size,
                     float left, float right, float baseline_y, PaintSink& sink);

} // namespace tessera::ui::paint::detail
