// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_paint.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::ui::text {

PaintedSegmentCursor::PaintedSegmentCursor(std::string_view text, std::size_t begin,
                                           std::size_t end, WhiteSpace mode,
                                           bool trim_edges) noexcept
    : text_(text),
      collapse_(white_space_collapses(mode)) {
    begin_ = std::min(begin, text.size());
    end_ = std::min(std::max(end, begin_), text.size());
    pos_ = begin_;
    if (trim_edges && white_space_trims_edges(mode)) {
        // The line edges are content for the trimming modes: a collapsible run only ever advances a
        // line between two units, so an edge run (the whitespace-only line box being the only one
        // the wrap keeps) paints nothing. `pre` and `break-spaces` never trim, so they keep it.
        while (begin_ < end_ && is_collapsible_space(text_[begin_])) {
            ++begin_;
        }
        while (end_ > begin_ && is_collapsible_space(text_[end_ - 1])) {
            --end_;
        }
        pos_ = begin_;
    }
    done_ = pos_ >= end_;
}

bool PaintedSegmentCursor::next(PaintedSegment& out) noexcept {
    if (done_ || pos_ >= end_) {
        done_ = true;
        return false;
    }
    if (!collapse_) {
        // Preserving modes paint the range verbatim: one run, so `pre`, `pre-wrap` and
        // `break-spaces` keep every preserved byte and advance exactly what the layout measured.
        out.source = text_.substr(pos_, end_ - pos_);
        out.source_begin = pos_;
        out.source_end = end_;
        out.collapsed = false;
        pos_ = end_;
        done_ = true;
        return true;
    }
    return next_collapsing(out);
}

bool PaintedSegmentCursor::next_collapsing(PaintedSegment& out) noexcept {
    if (pos_ >= end_) {
        done_ = true;
        return false;
    }
    if (is_collapsible_space(text_[pos_])) {
        const bool continues_before =
            pos_ == begin_ && begin_ > 0 && is_collapsible_space(text_[begin_ - 1]);
        if (continues_before) {
            // This run began before the range (a bidi visual-run boundary): only the range that
            // owns the run's first byte paints the collapsed space, so the line's painted
            // advance stays the collapsed one.
            while (pos_ < end_ && is_collapsible_space(text_[pos_])) {
                ++pos_;
            }
            if (pos_ >= end_) {
                done_ = true;
                return false;
            }
        } else {
            const std::size_t run_begin = pos_;
            while (pos_ < end_ && is_collapsible_space(text_[pos_])) {
                ++pos_;
            }
            out.source = std::string_view(&kCollapsedSpace, 1);
            out.source_begin = run_begin;
            out.source_end = pos_;
            out.collapsed = true;
            return true;
        }
    }
    // Content span. A single plain space stays in the span so ordinary prose paints as one run;
    // every other collapsible run (two or more spaces, or a tab/form feed) collapses to the
    // separate one-space segment above.
    const std::size_t content_begin = pos_;
    std::size_t cursor = pos_;
    while (cursor < end_) {
        if (!is_collapsible_space(text_[cursor])) {
            ++cursor;
            continue;
        }
        std::size_t run_end = cursor;
        while (run_end < end_ && is_collapsible_space(text_[run_end])) {
            ++run_end;
        }
        if (run_end - cursor == 1 && text_[cursor] == ' ') {
            cursor = run_end;
            continue;
        }
        break;
    }
    if (cursor == content_begin) {
        ++cursor;
    }
    pos_ = cursor;
    out.source = text_.substr(content_begin, cursor - content_begin);
    out.source_begin = content_begin;
    out.source_end = cursor;
    out.collapsed = false;
    return true;
}

} // namespace tessera::ui::text
