// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <string_view>

namespace tessera::ui::text {

// CSS collapsible whitespace. Only ASCII whitespace collapses or trims: UAX #14 keeps no-break and
// ideographic spaces inside a segment on purpose, and they must still advance the line.
[[nodiscard]] constexpr bool is_collapsible_space(char c) {
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
        return true;
    default:
        return false;
    }
}

// One painted run of a laid-out line (docs/design/text.md section 6.2). `source` is the UTF-8 the
// sink shapes; it is a view into the layout text, except for a collapsed run where it borrows the
// one-byte static space below. Nothing is owned and no allocation happens.
struct PaintedSegment {
    std::string_view source;
    // Source bytes this run covers. A collapsed run reports its whole source range, not the single
    // space `source` points at, so callers can map source offsets to painted runs.
    std::size_t source_begin = 0;
    std::size_t source_end = 0;
    bool collapsed = false;
};

// The single space a collapsed whitespace run paints. Static storage, so a PaintedSegment can point
// at it without copying.
inline constexpr char kCollapsedSpace = ' ';

// Sequential, allocation-free mapping from a laid-out line's source range to the painted segment
// sequence. The layout already trimmed the line edges for the trimming modes; `trim_edges` makes
// the cursor apply that rule itself, which is what a line-level call passes (it keeps a
// whitespace-only line box from painting its trimmed spaces). `pre` and `break-spaces` never trim,
// so the flag changes nothing for them. A bidi visual-run call passes false: a run boundary may cut
// a separator run, and only the run that owns the run's first byte paints the collapsed space.
class PaintedSegmentCursor {
public:
    PaintedSegmentCursor(std::string_view text, std::size_t begin, std::size_t end, WhiteSpace mode,
                         bool trim_edges = false) noexcept;

    // Writes the next segment and returns false once the range is exhausted.
    [[nodiscard]] bool next(PaintedSegment& out) noexcept;

private:
    [[nodiscard]] bool next_collapsing(PaintedSegment& out) noexcept;

    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t begin_ = 0;
    std::size_t end_ = 0;
    bool collapse_ = false;
    bool done_ = false;
};

} // namespace tessera::ui::text
