// SPDX-License-Identifier: MIT

#include "edit_grapheme.hpp"
#include "tessera/ui/text/hit_test.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace tessera::ui::text {
namespace {

float stub_advance(std::string_view run, const TextStyle& style, void* user) {
    const auto* metrics = static_cast<const FontMetrics*>(user);
    // The advance seam is literal: hit testing measures one cluster at a time, so a space cluster
    // must advance one space even when the line's mode collapses or trims separator runs.
    TextStyle literal = style;
    literal.white_space = WhiteSpace::Pre;
    return layout_text(run, literal, *metrics, TextConstraints{}).width;
}

struct Cursor {
    std::size_t offset = 0;
    float x = 0.0f;
};

// Cluster boundaries of one line with their measured visual x relative to the line start.
std::vector<Cursor> boundaries(std::string_view text, const TextLine& line, const TextStyle& style,
                               TextAdvanceMeasure measure, void* user) {
    std::vector<Cursor> result;
    std::size_t offset = line.begin;
    float x = 0.0f;
    result.push_back(Cursor{offset, x});
    while (offset < line.end) {
        const std::size_t next = edit_detail::next_cluster(text, offset);
        x += measure(text.substr(offset, next - offset), style, user);
        result.push_back(Cursor{next, x});
        offset = next;
    }
    return result;
}

TextHit nearest_ltr(const std::vector<Cursor>& cursors, float x) {
    if (cursors.empty()) {
        return {};
    }
    TextHit best{cursors.back().offset, CaretAffinity::Upstream};
    float best_distance = std::abs(x - cursors.back().x);
    for (const Cursor& cursor : cursors) {
        const float distance = std::abs(x - cursor.x);
        if (distance < best_distance) {
            best = TextHit{cursor.offset,
                           x < cursor.x ? CaretAffinity::Downstream : CaretAffinity::Upstream};
            best_distance = distance;
        }
    }
    return best;
}

TextHit nearest_rtl(const std::vector<Cursor>& cursors, float right_edge, float x) {
    if (cursors.empty()) {
        return {};
    }
    // Logical boundaries map to visual x = right_edge - prefix(width). At a tie choose the
    // Downstream boundary (before the following logical cluster).
    TextHit best{cursors.front().offset, CaretAffinity::Downstream};
    float best_distance = std::abs(x - (right_edge - cursors.front().x));
    bool first = true;
    for (const Cursor& cursor : cursors) {
        const float visual = right_edge - cursor.x;
        const float distance = std::abs(x - visual);
        if (first || distance < best_distance) {
            best = TextHit{cursor.offset,
                           x > visual ? CaretAffinity::Downstream : CaretAffinity::Upstream};
            best_distance = distance;
            first = false;
        }
    }
    return best;
}

} // namespace

TextHit hit_test_point(std::string_view text, const TextStyle& style,
                       const TextConstraints& constraints, float x, float y,
                       const FontMetrics& metrics, TextAdvanceMeasure measure, void* user) {
    if (measure == nullptr) {
        measure = &stub_advance;
        user = const_cast<FontMetrics*>(&metrics);
    }
    const TextLayout layout =
        layout_text_measured(text, style, metrics, constraints, measure, user);
    if (layout.lines.empty()) {
        return {};
    }
    std::size_t line_index = layout.lines.size() - 1;
    float top = 0.0f;
    for (std::size_t index = 0; index < layout.lines.size(); ++index) {
        const float bottom = top + layout.lines[index].height;
        if (y < bottom) {
            line_index = index;
            break;
        }
        top = bottom;
    }
    const TextLine& line = layout.lines[line_index];
    const float line_x = std::clamp(x, 0.0f, line.width);
    if (line.visual_runs.empty()) {
        return nearest_ltr(boundaries(text, line, style, measure, user), line_x);
    }
    float run_x = 0.0f;
    for (std::size_t index = 0; index < line.visual_runs.size(); ++index) {
        const BidiVisualRun& run = line.visual_runs[index];
        const float run_width = measure(text.substr(run.begin, run.end - run.begin), style, user);
        const float run_right = run_x + run_width;
        if (line_x <= run_right || index + 1 == line.visual_runs.size()) {
            if (index > 0 && line_x == run_x) {
                // Right-biased run-join affinity: the join belongs to the run whose logical range
                // ends there (the preceding visual run).
                return TextHit{line.visual_runs[index - 1].end, CaretAffinity::Upstream};
            }
            TextLine run_line;
            run_line.begin = run.begin;
            run_line.end = run.end;
            run_line.width = run_width;
            const auto cursors = boundaries(text, run_line, style, measure, user);
            if ((run.level & 1U) != 0U) {
                return nearest_rtl(cursors, run_width, line_x - run_x);
            }
            return nearest_ltr(cursors, line_x - run_x);
        }
        run_x = run_right;
    }
    return {line.end, CaretAffinity::Upstream};
}

} // namespace tessera::ui::text
