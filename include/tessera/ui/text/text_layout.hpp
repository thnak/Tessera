// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/bidi_types.hpp"
#include "tessera/ui/text/font_metrics.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tessera::ui::text {

struct TextLine {
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
    // Byte range of this line in the original UTF-8 text (hard newlines and inter-word separators
    // are excluded from both ends; a line's slice may contain the original inner separators).
    std::size_t begin = 0;
    std::size_t end = 0;
    // Visual (left-to-right) bidi runs. Empty means logical order is visual order (pure LTR/even
    // text), which keeps the existing paint path byte-identical.
    std::vector<BidiVisualRun> visual_runs;
};

struct TextLayout {
    std::vector<TextLine> lines;
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
    TextDirection direction = TextDirection::LeftToRight;
};

TextLayout layout_text(std::string_view text, const TextStyle& style, const FontMetrics& metrics,
                       const TextConstraints& constraints);

// Advance of one shaped run in pixels. The callback is the metrics seam for measured wrapping: the
// real-face path passes a HarfBuzz advance, the deterministic stub path uses layout_text directly.
using TextAdvanceMeasure = float (*)(std::string_view text, const TextStyle& style, void* user);

// Same greedy line-box algorithm as layout_text, but line widths come from `measure` instead of the
// fixed stub advance. Used by the paint wrap when a real font metrics source is supplied; the
// deterministic stub path keeps calling layout_text so goldens stay byte-identical.
TextLayout layout_text_measured(std::string_view text, const TextStyle& style,
                                const FontMetrics& metrics, const TextConstraints& constraints,
                                TextAdvanceMeasure measure, void* user);

} // namespace tessera::ui::text
