// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_layout.hpp"
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

// U+2026 HORIZONTAL ELLIPSIS, the one glyph `text-overflow: ellipsis` paints. Static view, so the
// truncating paint path copies and allocates nothing (docs/design/text.md section 6.3).
inline constexpr std::string_view kEllipsisUtf8 = "\xE2\x80\xA6";

// The truncation `text-overflow: ellipsis` applies to one laid-out line. `source_end` is the
// exclusive end of the surviving logical prefix (cluster-aligned); `ellipsis` says whether one
// `kEllipsisUtf8` run is painted immediately after its painted segments.
struct PaintedEllipsis {
    std::size_t source_end = 0;
    bool ellipsis = false;
};

// Fits `text-overflow: ellipsis` into `available_width` for the laid-out range [begin, end).
//
// The prefix is the longest cluster-aligned logical prefix whose *painted* advance fits, where the
// painted advance is the section 6.2 segment sequence (so a collapsed whitespace run costs one
// space, not its literal bytes) plus the one ellipsis this call reserves room for. The rules:
//   * the ellipsis is painted only when its own advance fits `available_width`; otherwise the
//     longest prefix that fits is painted with no ellipsis at all;
//   * the prefix may be empty (paint only the ellipsis) and the whole result never exceeds
//     `available_width`;
//   * nothing is rounded, measured twice, or allocated.
// `advance` is the metrics seam that measures one painted run (the same one paint shapes with).
// Callers only invoke this for a line whose laid-out width exceeds `available_width`; a line that
// fits is painted unchanged and never reaches this function.
[[nodiscard]] PaintedEllipsis fit_ellipsis_prefix(std::string_view text, std::size_t begin,
                                                  std::size_t end, WhiteSpace mode,
                                                  float available_width, const TextStyle& style,
                                                  TextAdvanceMeasure advance, void* user) noexcept;

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
