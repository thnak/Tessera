// SPDX-License-Identifier: MIT
//
// Glyph-run emission for one laid-out text line: the `white-space` paint-time segment transform
// (docs/design/text.md section 6.2), bidi visual-run order, and `text-overflow: ellipsis`
// (section 6.3). Paint consumes byte offsets and the line boxes the layout produced; it never
// shapes or lays out text itself.

#include "paint_text_runs.hpp"
#include "tessera/ui/text/bidi.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::ui::paint::detail {
namespace {

// Advance callback for the text module's measurement seams (bidi offset mapping and the
// `text-overflow: ellipsis` fit): both measure one painted run with this paint's metrics source.
float run_advance(std::string_view text, const text::TextStyle& style, void* user) {
    return static_cast<const PaintTextMetricsSource*>(user)->advance(text, style);
}

// Emits the painted segment sequence of one source range (docs/design/text.md section 6.2),
// advancing `x`. `single_width` is set when the range is a whole line: if it paints as one run the
// laid-out box width is authoritative, so the painted advance equals the measured line width for
// every metrics source (the sink still shapes `source`).
void emit_range_segments(std::string_view text, const text::TextLine& line, std::size_t begin,
                         std::size_t end, bool trim_edges, std::optional<float> single_width,
                         const text::TextStyle& text_style, const PaintTextMetricsSource& source,
                         float& x, float line_top, const rhi::Color& color, PaintSink& sink,
                         PaintStats& stats) {
    text::PaintedSegmentCursor cursor(text, begin, end, text_style.white_space, trim_edges);
    text::PaintedSegment first;
    if (!cursor.next(first)) {
        return;
    }
    text::PaintedSegment second;
    const bool has_second = cursor.next(second);
    const bool single = !has_second && first.source_begin == begin && first.source_end == end;

    const auto emit = [&](const text::PaintedSegment& segment, float width) {
        // `text-transform` is applied to the emitted bytes as well as to `PaintTextMetricsSource::
        // advance`, so the painted glyph sequence and the advance the cursor uses are the same
        // bytes. The mapping is 1:1 (ASCII only), so the segment's source offsets and the line's
        // width are untouched, and a line's segment advances still sum to its laid-out width. V1
        // cost: one transformed copy per emitted segment while a transform is active; hoisting that
        // into a caller-owned scratch buffer is a follow-up if a transformed hot scene needs it.
        std::string transformed;
        std::string_view painted = segment.source;
        if (text_style.text_transform != text::TextTransform::None) {
            transformed = text::apply_text_transform(segment.source, text_style.text_transform);
            painted = transformed;
        }
        TextDraw run;
        run.rect = {std::round(x), std::round(line_top), width, line.height};
        run.text = painted;
        run.font_size = text_style.font_size;
        run.font_weight = text_style.font_weight;
        run.color = color;
        run.baseline = std::round(line_top + line.baseline);
        sink.draw_text(run);
        ++stats.texts;
        x += width;
    };
    if (single) {
        emit(first, single_width.value_or(source.advance(first.source, text_style)));
        return;
    }
    emit(first, source.advance(first.source, text_style));
    if (!has_second) {
        return;
    }
    emit(second, source.advance(second.source, text_style));
    while (cursor.next(second)) {
        emit(second, source.advance(second.source, text_style));
    }
}

// Paints one `text-overflow: ellipsis` line: the fitted logical prefix, emitted through the same
// section 6.2 segment sequence, followed by exactly one U+2026 when the fit reserved room for it.
// Bidi lines keep their visual-run order: each surviving run is clipped to the logical cut, so a
// run that begins after it paints nothing. Nothing here allocates or lays out again.
void emit_ellipsized_line(const text::TextLine& line, std::string_view text,
                          const text::PaintedEllipsis& fit, const text::TextStyle& text_style,
                          const PaintTextMetricsSource& source, float line_x, float line_top,
                          const rhi::Color& color, PaintSink& sink, PaintStats& stats) {
    const std::size_t cut = std::min(std::max(fit.source_end, line.begin), line.end);
    float x = line_x;
    // The cut is not a line edge: the layout already trimmed the line's real edges, and the fit
    // counted the segment sequence up to `cut`, so the truncated range must not trim again (a cut
    // that lands after a collapsed run keeps the one space the fit reserved).
    if (line.visual_runs.empty()) {
        emit_range_segments(text, line, line.begin, cut, false, std::nullopt, text_style, source, x,
                            line_top, color, sink, stats);
    } else {
        for (const text::BidiVisualRun& visual_run : line.visual_runs) {
            if (visual_run.end <= visual_run.begin || visual_run.begin >= cut) {
                continue;
            }
            emit_range_segments(text, line, visual_run.begin, std::min(visual_run.end, cut), false,
                                std::nullopt, text_style, source, x, line_top, color, sink, stats);
        }
    }
    if (!fit.ellipsis) {
        return;
    }
    TextDraw ellipsis;
    const float width = source.advance(text::kEllipsisUtf8, text_style);
    ellipsis.rect = {std::round(x), std::round(line_top), width, line.height};
    ellipsis.text = text::kEllipsisUtf8;
    ellipsis.font_size = text_style.font_size;
    ellipsis.font_weight = text_style.font_weight;
    ellipsis.color = color;
    ellipsis.baseline = std::round(line_top + line.baseline);
    sink.draw_text(ellipsis);
    ++stats.texts;
}

} // namespace

float line_offset_x(std::string_view text, const text::TextLine& line, std::size_t offset,
                    const text::TextStyle& style, const PaintTextMetricsSource& source) {
    if (line.visual_runs.empty()) {
        const std::size_t clamped = std::min(std::max(offset, line.begin), line.end);
        float x = 0.0f;
        text::PaintedSegmentCursor cursor(text, line.begin, clamped, style.white_space);
        text::PaintedSegment segment;
        while (cursor.next(segment)) {
            x += source.advance(segment.source, style);
        }
        return x;
    }
    return text::bidi_offset_x(text, line, offset, style, &run_advance,
                               const_cast<PaintTextMetricsSource*>(&source));
}

void emit_line_runs(const text::TextLine& line, std::string_view text,
                    const text::TextStyle& text_style, const PaintTextMetricsSource& source,
                    float line_x, float line_top, float available_width, const rhi::Color& color,
                    PaintSink& sink, PaintStats& stats) {
    if (line.end <= line.begin) {
        return;
    }
    if (text_style.text_overflow == text::TextOverflow::Ellipsis && line.width > available_width) {
        // Paint-side only: the fit reuses the metrics seam and the section 6.2 views, so layout,
        // line boxes, baselines and the measure cache stay exactly what `clip` produces.
        const text::PaintedEllipsis fit = text::fit_ellipsis_prefix(
            text, line.begin, line.end, text_style.white_space, available_width, text_style,
            &run_advance, const_cast<PaintTextMetricsSource*>(&source));
        emit_ellipsized_line(line, text, fit, text_style, source, line_x, line_top, color, sink,
                             stats);
        return;
    }
    if (line.visual_runs.empty()) {
        float x = line_x;
        emit_range_segments(text, line, line.begin, line.end, true, line.width, text_style, source,
                            x, line_top, color, sink, stats);
        return;
    }
    float x = line_x;
    for (const text::BidiVisualRun& visual_run : line.visual_runs) {
        if (visual_run.end <= visual_run.begin) {
            continue;
        }
        emit_range_segments(text, line, visual_run.begin, visual_run.end, false, std::nullopt,
                            text_style, source, x, line_top, color, sink, stats);
    }
}

} // namespace tessera::ui::paint::detail
