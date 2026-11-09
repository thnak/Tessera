// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tessera::ui::text {

// Paragraph direction by the UBA P2/P3 first-strong heuristic (strong L / R / AL). Text with no
// strong character is LTR. There is no explicit per-run direction override in the paint payload:
// RLM/LRM/embedding characters affect SheenBidi naturally, and explicit CSS `direction` is not
// wired yet (documented deferral).
[[nodiscard]] TextDirection bidi_paragraph_direction(std::string_view utf8);

// Fills `lines[i].visual_runs` from the resolved bidi line runs (SheenBidi, visual order). For pure
// LTR/even text whose runs are already in logical order the vector is left empty, which keeps the
// existing logical LTR paint path byte-identical. `paragraph_direction` selects the base level.
void bidi_apply_visual_runs(std::string_view utf8, TextDirection paragraph_direction,
                            std::vector<TextLine>& lines);

// Advance callback used to convert a run-local byte offset to pixels.
using BidiAdvance = float (*)(std::string_view text, const TextStyle& style, void* user);

// Visual x of a logical byte offset relative to the line start, for a line with visual runs.
// Boundary affinity: at a logical run join the offset belongs to the run whose logical range ends
// there (right-biased), so a caret at a join is drawn at the visual end of the preceding logical
// run; line begin belongs to the first logical run. RTL runs map from their right edge.
[[nodiscard]] float bidi_offset_x(std::string_view utf8, const TextLine& line,
                                  std::size_t logical_offset, const TextStyle& style,
                                  BidiAdvance advance, void* user);

} // namespace tessera::ui::text
