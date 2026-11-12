// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "text_breaks.hpp"

#include <string_view>

// Greedy line-box layout driven by an explicit break engine (docs/design/text.md section 6).
//
// The public layout_text/layout_text_measured entry points call this with active_backend(), so the
// TESSERA_ICU_REDUCED_DATA switch is the only thing that picks the engine. The parity test drives
// both engines through this entry point, which is why it takes the backend instead of reading it.
// TextStyle::white_space selects what a collapsible whitespace run means between line units:
// `normal` collapses it to one space advance, `nowrap` collapses it on one edge-trimmed line,
// `pre` preserves it literally on one line, `pre-wrap` advances it literally, `break-spaces`
// makes every preserved space its own unit.
namespace tessera::ui::text {

// `measure` is the raw advance seam of TextLayout: nullptr keeps the deterministic stub advance
// (font-size / 2 + letter spacing per code point), otherwise the callback supplies a run advance
// without `word-spacing`; the wrap adds that per painted separator for both paths.
[[nodiscard]] TextLayout
layout_text_with_breaks(std::string_view text, const TextStyle& style, const FontMetrics& metrics,
                        const TextConstraints& constraints, BreakBackend backend,
                        TextAdvanceMeasure measure = nullptr, void* user = nullptr);

} // namespace tessera::ui::text
