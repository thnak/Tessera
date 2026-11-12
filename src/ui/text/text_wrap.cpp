// SPDX-License-Identifier: MIT

#include "tessera/ui/text/bidi.hpp"
#include "tessera/ui/text/text_paint.hpp"
#include "text_wrap.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::text {
namespace {

// One UAX #14 segment between two legal break opportunities: the unit the greedy wrap places as a
// whole. LB18 attaches a whitespace run to the segment before it, so the raw range ends in the
// trailing spaces; `core_begin`/`core_end` exclude them at both edges because a trailing space
// never advances a line and a leading one is trimmed at a line start. A whitespace-only segment has
// core_begin == core_end and is skipped by the wrap unless it sits between two segments. The
// break-spaces mode builds its units differently (see break_space_units): nothing is trimmed there.
struct Segment {
    std::size_t core_begin = 0;
    std::size_t core_end = 0;
};

std::size_t codepoint_count(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

std::size_t byte_offset(std::string_view text, std::size_t codepoints) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < text.size() && count < codepoints) {
        ++offset;
        while (offset < text.size() &&
               (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u) {
            ++offset;
        }
        ++count;
    }
    return offset;
}

// break-spaces units: the paragraph is preserved verbatim, and every preserved ASCII whitespace
// character becomes its own line unit. CSS puts a break opportunity after every preserved space,
// so a line may start with spaces, end with them, or consist only of them; the greedy fill keeps
// as many spaces as fit and moves the rest to the next line. The CR of a CRLF pair is dropped
// because CSS preprocessing folds it into the segment break and it never advances a line.
std::vector<Segment> break_space_units(std::string_view text, std::size_t begin, std::size_t end) {
    std::vector<Segment> units;
    std::size_t cursor = begin;
    while (cursor < end) {
        if (text[cursor] == '\r') {
            ++cursor;
            continue;
        }
        // ASCII whitespace is one byte and UTF-8 continuation bytes are never ASCII, so a byte
        // walk keeps multi-byte runs intact.
        if (is_collapsible_space(text[cursor])) {
            units.push_back(Segment{cursor, cursor + 1});
            ++cursor;
            continue;
        }
        std::size_t stop = cursor + 1;
        while (stop < end && !is_collapsible_space(text[stop])) {
            ++stop;
        }
        units.push_back(Segment{cursor, stop});
        cursor = stop;
    }
    return units;
}

std::vector<Segment> paragraph_segments(std::string_view text, std::size_t begin, std::size_t end,
                                        BreakBackend backend, WhiteSpace mode) {
    if (white_space_breaks_after_spaces(mode)) {
        return break_space_units(text, begin, end);
    }
    std::vector<Segment> segments;
    const std::size_t size = end - begin;
    const BreakBoundaries boundaries = line_boundaries(text.substr(begin, size), backend);
    std::size_t previous = begin;
    for (std::size_t index = 1; index < boundaries.offsets.size(); ++index) {
        const std::size_t offset = boundaries.offsets[index];
        const std::size_t at = begin + std::min(offset, size);
        if (at <= previous) {
            continue;
        }
        Segment segment{previous, at};
        // Normal and pre-wrap trim both edges. NoWrap never reaches this loop (the caller takes the
        // single-line path) and break-spaces returned above, so the guard documents the contract.
        if (white_space_trims_edges(mode)) {
            while (segment.core_begin < segment.core_end &&
                   is_collapsible_space(text[segment.core_begin])) {
                ++segment.core_begin;
            }
            while (segment.core_end > segment.core_begin &&
                   is_collapsible_space(text[segment.core_end - 1])) {
                --segment.core_end;
            }
        }
        segments.push_back(segment);
        previous = at;
    }
    return segments;
}

// Largest legal line-break opportunity at or before `fitting_bytes` inside `run` (0 when the run
// has none). Only segments wider than the constraint need this: a normal segment is already a
// legal line, so this is where a hyphen, CJK ideograph, or ZWSP turns a mid-run hard split into a
// linguistically valid break.
std::size_t legal_chunk_end(std::string_view run, std::size_t fitting_bytes, BreakBackend backend) {
    const BreakBoundaries boundaries = line_boundaries(run, backend);
    std::size_t best = 0;
    for (const EditOffset at : boundaries.offsets) {
        if (at == 0) {
            continue;
        }
        if (at > fitting_bytes) {
            break;
        }
        best = at;
    }
    return best;
}

TextLayout finalize_bidi(TextLayout layout, std::string_view text) {
    layout.direction = bidi_paragraph_direction(text);
    bidi_apply_visual_runs(text, layout.direction, layout.lines);
    return layout;
}

// One collapsing single line (CSS `normal` with wrapping disabled, and `nowrap`): leading and
// trailing collapsible runs are trimmed and every interior run measures as one space advance,
// exactly what the wrapped path adds per segment gap. Allocation-free: one measure call per content
// span and one per interior run.
struct CollapsedLine {
    std::size_t begin = 0;
    std::size_t end = 0;
    float width = 0.0f;
};

template<typename Measure>
CollapsedLine measure_collapsed_line(std::string_view text, Measure&& measure) {
    CollapsedLine line;
    const std::size_t size = text.size();
    std::size_t pos = 0;
    while (pos < size && is_collapsible_space(text[pos])) {
        ++pos;
    }
    line.begin = pos;
    line.end = pos;
    while (pos < size) {
        const std::size_t content_begin = pos;
        while (pos < size && !is_collapsible_space(text[pos])) {
            ++pos;
        }
        line.width += measure(text.substr(content_begin, pos - content_begin));
        line.end = pos;
        if (pos >= size) {
            break;
        }
        while (pos < size && is_collapsible_space(text[pos])) {
            ++pos;
        }
        if (pos < size) {
            line.width += measure(" ");
        }
    }
    return line;
}

// The greedy wrap over the mode's line units (docs/design/text.md section 6). One unit is the
// smallest line unit; a unit wider than the constraint is split at the last measured code-point
// prefix that fits, then at the last legal break inside that prefix when one exists.
template<typename Measure>
TextLayout wrap_with(std::string_view text, const TextStyle& style, const FontMetrics& metrics,
                     const TextConstraints& constraints, BreakBackend backend, Measure&& measure) {
    TextLayout result;
    const float line_height = style.line_height > 0.0f
                                  ? style.line_height
                                  : metrics.ascent + metrics.descent + metrics.line_gap;
    // `word-spacing` is additive to every separator advance the wrap emits: one per collapsed run
    // (its one painted space), one per preserved space byte (U+0020), and one per space inside a
    // whole-run measure. Trimming already removed the line edges, so a space that is not painted
    // never widens anything, and tabs and hard newlines are never counted (word_separator_count).
    const float word_spacing = style.word_spacing > 0.0f ? style.word_spacing : 0.0f;
    const auto measure_run = [&](std::string_view run) {
        return measure(run) + word_spacing * static_cast<float>(word_separator_count(run));
    };

    auto add_line = [&](float width, std::size_t begin, std::size_t end) {
        result.lines.push_back(TextLine{width, line_height, metrics.ascent, begin, end, {}});
        result.width = std::max(result.width, width);
    };

    if (!constraints.wrap || !white_space_wraps(style.white_space) ||
        !std::isfinite(constraints.max_width)) {
        // The single-line path. Collapsing modes (`normal` with wrapping disabled, `nowrap`) trim
        // the edges and measure every interior run as one space; `pre`, `pre-wrap` and
        // `break-spaces` keep the previous literal single line.
        if (white_space_collapses(style.white_space)) {
            const CollapsedLine line = measure_collapsed_line(text, measure_run);
            add_line(line.width, line.begin, line.end);
        } else {
            add_line(measure_run(text), 0, text.size());
        }
        result.height = line_height;
        result.baseline = metrics.ascent;
        return finalize_bidi(std::move(result), text);
    }

    const float max_width = std::max(0.1f, constraints.max_width);
    const bool collapse_gap = white_space_collapses(style.white_space);
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t newline = text.find('\n', begin);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::size_t lines_before = result.lines.size();

        float current = 0.0f;
        bool has_line = false;
        std::size_t line_begin = begin;
        std::size_t line_end = begin;
        for (const Segment& segment :
             paragraph_segments(text, begin, end, backend, style.white_space)) {
            if (segment.core_begin == segment.core_end) {
                // Whitespace only: trimmed at a line start, and never the content that starts a
                // line. Between two segments its bytes are measured as the gap below.
                continue;
            }
            const float width =
                measure_run(text.substr(segment.core_begin, segment.core_end - segment.core_begin));
            float gap = 0.0f;
            if (has_line) {
                // Interior separators. `normal` collapses a run to one space advance; `pre-wrap`
                // advances the run literally. break-spaces has no gaps: its spaces are units.
                if (collapse_gap) {
                    gap = segment.core_begin > line_end ? measure_run(" ") : 0.0f;
                } else {
                    gap = measure_run(text.substr(line_end, segment.core_begin - line_end));
                }
            }
            if (has_line && current + gap + width <= max_width) {
                current += gap + width;
                line_end = segment.core_end;
                continue;
            }
            if (has_line) {
                add_line(current, line_begin, line_end);
                has_line = false;
            }
            if (width > max_width) {
                std::size_t chunk_begin = segment.core_begin;
                while (chunk_begin < segment.core_end) {
                    const std::string_view remaining =
                        text.substr(chunk_begin, segment.core_end - chunk_begin);
                    std::size_t best = 1;
                    std::size_t low = 1;
                    std::size_t high = codepoint_count(remaining);
                    while (low <= high) {
                        const std::size_t middle = low + (high - low) / 2;
                        const float prefix =
                            measure_run(remaining.substr(0, byte_offset(remaining, middle)));
                        if (prefix <= max_width) {
                            best = middle;
                            low = middle + 1;
                        } else if (middle == 1) {
                            break;
                        } else {
                            high = middle - 1;
                        }
                    }
                    const std::size_t fitting = byte_offset(remaining, best);
                    const std::size_t legal = legal_chunk_end(remaining, fitting, backend);
                    const std::size_t chunk_bytes = legal != 0 ? legal : fitting;
                    add_line(measure_run(remaining.substr(0, chunk_bytes)), chunk_begin,
                             chunk_begin + chunk_bytes);
                    chunk_begin += chunk_bytes;
                }
                continue;
            }
            current = width;
            line_begin = segment.core_begin;
            line_end = segment.core_end;
            has_line = true;
        }
        if (has_line) {
            add_line(current, line_begin, line_end);
        }
        if (result.lines.size() == lines_before) {
            // A blank (or whitespace-only) paragraph still owns a line box, so a caret after Enter
            // and a trailing newline land on their own line instead of the previous one.
            add_line(0.0f, begin, end);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        begin = newline + 1;
    }

    result.height = static_cast<float>(result.lines.size()) * line_height;
    result.baseline = metrics.ascent;
    return finalize_bidi(std::move(result), text);
}

} // namespace

TextLayout layout_text_with_breaks(std::string_view text, const TextStyle& style,
                                   const FontMetrics& metrics, const TextConstraints& constraints,
                                   BreakBackend backend, TextAdvanceMeasure measure, void* user) {
    if (text.empty()) {
        return {};
    }
    if (measure != nullptr) {
        return wrap_with(text, style, metrics, constraints, backend, [&](std::string_view run) {
            return std::max(0.0f, measure(run, style, user));
        });
    }
    const float advance = std::max(0.1f, style.font_size * 0.5f + style.letter_spacing);
    return wrap_with(text, style, metrics, constraints, backend, [advance](std::string_view run) {
        return static_cast<float>(codepoint_count(run)) * advance;
    });
}

} // namespace tessera::ui::text
