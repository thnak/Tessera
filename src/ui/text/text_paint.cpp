// SPDX-License-Identifier: MIT

#include "edit_grapheme.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::ui::text {
namespace {

// End (source offset) of the longest prefix of one painted segment whose advance fits `budget`.
// The binary search walks byte lengths because the advance is monotonic in the prefix, then snaps
// down to a grapheme-cluster boundary: paint never splits a multi-byte character or a base + mark
// cluster, and the measurement used for the decision is the caller's metric seam.
std::size_t fitting_prefix_end(std::string_view text, const PaintedSegment& segment,
                               const TextStyle& style, TextAdvanceMeasure advance, void* user,
                               float budget) noexcept {
    if (segment.collapsed) {
        // A collapsed run paints as exactly one space: either that space fits and the run's whole
        // source range is consumed, or the prefix stops before it.
        return advance(segment.source, style, user) <= budget ? segment.source_end
                                                              : segment.source_begin;
    }
    std::size_t low = 0;
    std::size_t high = segment.source.size();
    while (low < high) {
        const std::size_t middle = low + (high - low + 1) / 2;
        if (advance(segment.source.substr(0, middle), style, user) <= budget) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    const std::size_t snapped = edit_detail::snap_to_cluster(text, segment.source_begin + low);
    std::size_t cut = snapped <= segment.source_begin ? 0 : snapped - segment.source_begin;
    // A truncated UTF-8 prefix can measure shorter than the cluster it belongs to (a shaper drops
    // the broken tail), so re-measure and step back cluster by cluster until the prefix really
    // fits the budget.
    while (cut > 0 && advance(segment.source.substr(0, cut), style, user) > budget) {
        cut = edit_detail::prev_cluster(text, segment.source_begin + cut) - segment.source_begin;
    }
    return segment.source_begin + cut;
}

} // namespace

PaintedEllipsis fit_ellipsis_prefix(std::string_view text, std::size_t begin, std::size_t end,
                                    WhiteSpace mode, float available_width, const TextStyle& style,
                                    TextAdvanceMeasure advance, void* user) noexcept {
    PaintedEllipsis result;
    begin = std::min(begin, text.size());
    end = std::min(std::max(end, begin), text.size());
    result.source_end = begin;
    if (advance == nullptr || end <= begin || !(available_width > 0.0f)) {
        return result;
    }
    // The ellipsis is reserved first: it is painted only when it fits, and the prefix then gets
    // whatever is left. `available_width < 0` and NaN already returned above.
    const float ellipsis_width = std::max(0.0f, advance(kEllipsisUtf8, style, user));
    result.ellipsis = ellipsis_width <= available_width;
    const float budget = result.ellipsis ? available_width - ellipsis_width : available_width;

    // Walk the painted sequence once (section 6.2): segments tile the line range, so the cut maps
    // back to a source byte. Only the segment that straddles the budget is searched further.
    PaintedSegmentCursor cursor(text, begin, end, mode, true);
    PaintedSegment segment;
    float used = 0.0f;
    std::size_t cut = begin;
    while (cursor.next(segment)) {
        const float width = std::max(0.0f, advance(segment.source, style, user));
        if (used + width <= budget) {
            used += width;
            cut = segment.source_end;
            continue;
        }
        if (budget > used) {
            cut = fitting_prefix_end(text, segment, style, advance, user, budget - used);
        }
        break;
    }
    result.source_end = cut;
    return result;
}

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
