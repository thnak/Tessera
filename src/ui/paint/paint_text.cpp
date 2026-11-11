// SPDX-License-Identifier: MIT
//
// PaintData text payload emission: single-run, wrapped and bidi glyph runs, plus the caret,
// selection and IME preedit decoration. Paint consumes byte offsets and resolves them through the
// same line boxes the layout produced; it never shapes or lays out text itself.

#include "paint_properties.hpp"
#include "paint_text.hpp"
#include "tessera/ui/text/bidi.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::paint::detail {
namespace {

// Paints an optional PaintData text run (D9) after the node's background/borders and inside the
// active clip. Precedence: PaintData override > computed style > backend default; node opacity
// multiplies the effective color alpha. NoWrap (default) emits one run; Wrap uses the shared
// text-line-box layout (`text::layout_text`) with the content box width and emits one run per line.
PaintTextAlign computed_text_align(const style::ComputedStyle& style) {
    const std::string* value = style.find("text-align");
    if (value == nullptr) {
        return PaintTextAlign::Start;
    }
    std::string token(*value);
    std::size_t first = 0;
    while (first < token.size() && std::isspace(static_cast<unsigned char>(token[first])) != 0) {
        ++first;
    }
    std::size_t last = token.size();
    while (last > first && std::isspace(static_cast<unsigned char>(token[last - 1])) != 0) {
        --last;
    }
    token = token.substr(first, last - first);
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (token == "center") {
        return PaintTextAlign::Center;
    }
    if (token == "end" || token == "right") {
        return PaintTextAlign::End;
    }
    return PaintTextAlign::Start;
}

// Caret/selection emission. Paint only consumes byte offsets: it resolves them through the same
// line boxes the wrap path uses. Z-order: selection fills behind the glyph runs, caret fills after
// them. Selection is a translucent fill derived from the text color; the caret is the text color at
// full alpha, 1 px at device scale 1 and 2 px at scale >= 2. A blink phase below 0.5 hides only the
// caret; paint has no timers.
// Advance callback for bidi caret mapping: the metrics source used for this paint.
float bidi_advance(std::string_view text, const text::TextStyle& style, void* user) {
    return static_cast<const PaintTextMetricsSource*>(user)->advance(text, style);
}

// Visual x of a logical byte offset relative to the line start. Uses the bidi run order/affinity
// when the line has visual runs; pure LTR lines sum the painted segments of the prefix, so a
// collapsed interior run costs the one space advance the layout measured.
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
    return text::bidi_offset_x(text, line, offset, style, &bidi_advance,
                               const_cast<PaintTextMetricsSource*>(&source));
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
        TextDraw run;
        run.rect = {std::round(x), std::round(line_top), width, line.height};
        run.text = segment.source;
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

// Emits one line's glyph runs left-to-right in visual order, applying the mode's paint-time
// segment transform: `normal`/`nowrap` paint a collapsed run as the one space advance the layout
// measured, while `pre`, `pre-wrap` and `break-spaces` keep every preserved byte in one run. A
// bidi line applies the same transform per visual run, and only the run that owns a separator
// run's first byte paints its collapsed space.
void emit_line_runs(const text::TextLine& line, std::string_view text,
                    const text::TextStyle& text_style, const PaintTextMetricsSource& source,
                    float line_x, float line_top, const rhi::Color& color, PaintSink& sink,
                    PaintStats& stats) {
    if (line.end <= line.begin) {
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

void paint_text_decoration(const PaintData& data, const rhi::Rect& content,
                           const text::TextStyle& text_style, const rhi::Color& color,
                           float opacity, bool wrap, PaintTextAlign align, PaintSink& sink,
                           PaintStats& stats, const PaintContext& context) {
    const std::string& text = data.text;
    StubPaintTextMetrics stub_source;
    const PaintTextMetricsSource* metrics_source =
        context.text_metrics_source != nullptr ? context.text_metrics_source : &stub_source;

    text::TextConstraints constraints;
    constraints.wrap = wrap;
    constraints.max_width = content.width;
    text::TextLayout local_layout = metrics_source->layout(text, text_style, constraints);
    if (data.max_lines > 0 && local_layout.lines.size() > data.max_lines) {
        local_layout.lines.resize(data.max_lines);
    }
    if (local_layout.lines.empty()) {
        // An active IME preedit must render even when the committed text is empty (there is no
        // glyph run to borrow a line box from): synthesize the same line box the text layout would
        // produce, using the metrics source (paint still does not shape or lay out the preedit).
        if (!data.decoration || !data.decoration->composition_active) {
            return;
        }
        const text::FontMetrics fallback = metrics_source->metrics(text_style);
        text::TextLine line;
        line.height = fallback.ascent + fallback.descent + fallback.line_gap;
        line.baseline = fallback.ascent;
        local_layout.lines.push_back(line);
    }

    struct LineGeometry {
        text::TextLine line;
        float x = 0.0f;
        float top = 0.0f;
    };
    std::vector<LineGeometry> lines;
    lines.reserve(local_layout.lines.size());
    float line_top = content.y;
    for (const text::TextLine& line : local_layout.lines) {
        float line_x = content.x;
        if (align == PaintTextAlign::Center) {
            line_x += (content.width - line.width) * 0.5f;
        } else if (align == PaintTextAlign::End) {
            line_x += content.width - line.width;
        }
        lines.push_back(LineGeometry{line, std::max(content.x, line_x), line_top});
        line_top += line.height;
    }

    const TextDecoration& decoration = *data.decoration;
    std::size_t selection_begin = std::min(decoration.selection_begin, text.size());
    std::size_t selection_end = std::min(decoration.selection_end, text.size());
    if (selection_begin > selection_end) {
        std::swap(selection_begin, selection_end);
    }
    rhi::Color selection_color = color;
    selection_color.a = 0.35f * opacity; // fixed documented alpha; text color supplies RGB
    if (selection_end > selection_begin && selection_color.a > 0.0f) {
        for (const LineGeometry& geometry : lines) {
            const text::TextLine& line = geometry.line;
            if (line.visual_runs.empty()) {
                const std::size_t start = std::max(selection_begin, line.begin);
                const std::size_t end = std::min(selection_end, line.end);
                if (end <= start) {
                    continue;
                }
                const float start_x = line_offset_x(text, line, start, text_style, *metrics_source);
                const float end_x = line_offset_x(text, line, end, text_style, *metrics_source);
                sink.fill_rect({std::round(geometry.x + std::min(start_x, end_x)),
                                std::round(geometry.top), std::round(std::abs(end_x - start_x)),
                                line.height},
                               selection_color);
                continue;
            }
            // One rectangle per visual run: a selection spanning a direction change is the union of
            // the run-local visual intervals. Selection mapping uses run-local affinity (RTL run
            // logical start is its visual right edge), unlike the caret's global join affinity.
            float run_x = geometry.x;
            for (const text::BidiVisualRun& visual_run : line.visual_runs) {
                const std::string_view run_text = std::string_view(text).substr(
                    visual_run.begin, visual_run.end - visual_run.begin);
                const float run_width = metrics_source->advance(run_text, text_style);
                const std::size_t start = std::max(selection_begin, visual_run.begin);
                const std::size_t end = std::min(selection_end, visual_run.end);
                if (end > start) {
                    const float start_prefix = metrics_source->advance(
                        std::string_view(text).substr(visual_run.begin, start - visual_run.begin),
                        text_style);
                    const float end_prefix = metrics_source->advance(
                        std::string_view(text).substr(visual_run.begin, end - visual_run.begin),
                        text_style);
                    float x0 = run_x + start_prefix;
                    float x1 = run_x + end_prefix;
                    if ((visual_run.level & 1U) != 0U) {
                        x0 = run_x + run_width - start_prefix;
                        x1 = run_x + run_width - end_prefix;
                    }
                    sink.fill_rect({std::round(std::min(x0, x1)), std::round(geometry.top),
                                    std::round(std::abs(x1 - x0)), line.height},
                                   selection_color);
                }
                run_x += run_width;
            }
        }
    }

    // Glyph runs: selection above, text now (per line so the geometry matches the decoration).
    for (const LineGeometry& geometry : lines) {
        emit_line_runs(geometry.line, text, text_style, *metrics_source, geometry.x, geometry.top,
                       color, sink, stats);
    }

    // IME preedit: underline the composition range and, when the preedit is not part of the painted
    // text, draw the uncommitted run at the composition anchor. Z-order: after the glyph runs (so
    // above the selection), before the caret. The run uses the committed text's color (node opacity
    // already folded); the underline is the text color, 1 px at device scale 1 and 2 px at >= 2.
    // The byte range -> line-box mapping is the same one the selection/caret use.
    if (decoration.composition_active) {
        const std::size_t begin = std::min(decoration.preedit_begin, text.size());
        const std::size_t end = std::min(decoration.preedit_end, text.size());
        const std::size_t range_begin = std::min(begin, end);
        const std::size_t range_end = std::max(begin, end);
        const float thickness = context.device_scale >= 2.0f ? 2.0f : 1.0f;

        const LineGeometry* anchor = &lines.back();
        for (const LineGeometry& geometry : lines) {
            if (range_begin >= geometry.line.begin && range_begin <= geometry.line.end) {
                anchor = &geometry;
                break;
            }
        }
        const std::size_t clamped = std::min(range_begin, anchor->line.end);
        const float prefix =
            line_offset_x(text, anchor->line, clamped, text_style, *metrics_source);
        const float x = std::round(anchor->x + prefix);
        const float top = std::round(anchor->top);

        if (!decoration.preedit_text.empty()) {
            TextDraw run;
            run.rect = {x, top,
                        std::round(metrics_source->advance(decoration.preedit_text, text_style)),
                        anchor->line.height};
            run.text = decoration.preedit_text;
            run.font_size = text_style.font_size;
            run.font_weight = text_style.font_weight;
            run.color = color;
            run.baseline = std::round(anchor->top + anchor->line.baseline);
            sink.draw_text(run);
            ++stats.texts;
            if (decoration.preedit_underline) {
                sink.fill_rect({x, std::round(anchor->top + anchor->line.height - thickness),
                                run.rect.width, thickness},
                               color);
            }
        } else if (decoration.preedit_underline) {
            // The composition is already in the painted text: underline only the replaced range.
            for (const LineGeometry& geometry : lines) {
                const text::TextLine& line = geometry.line;
                const std::size_t start = std::max(range_begin, line.begin);
                const std::size_t stop = std::min(range_end, line.end);
                if (stop <= start) {
                    continue;
                }
                const float start_width =
                    line_offset_x(text, line, start, text_style, *metrics_source);
                const float stop_width =
                    line_offset_x(text, line, stop, text_style, *metrics_source);
                const float left = std::min(start_width, stop_width);
                const float right = std::max(start_width, stop_width);
                sink.fill_rect({std::round(geometry.x + left),
                                std::round(geometry.top + line.height - thickness),
                                std::round(right - left), thickness},
                               color);
            }
        }
    }

    const bool caret_visible = decoration.caret_visible && decoration.blink_phase >= 0.5f;
    if (caret_visible) {
        const std::size_t caret = std::min(decoration.caret_byte_offset, text.size());
        const LineGeometry* caret_line = &lines.back();
        for (const LineGeometry& geometry : lines) {
            if (caret >= geometry.line.begin && caret <= geometry.line.end) {
                caret_line = &geometry;
                break;
            }
        }
        const std::size_t clamped = std::min(caret, caret_line->line.end);
        const float advance =
            line_offset_x(text, caret_line->line, clamped, text_style, *metrics_source);
        const float caret_width = context.device_scale >= 2.0f ? 2.0f : 1.0f;
        sink.fill_rect({std::round(caret_line->x + advance), std::round(caret_line->top),
                        std::max(caret_width, 1.0f), caret_line->line.height},
                       color);
    }
}

} // namespace

void paint_text_lines(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                      PaintSink& sink, PaintStats& stats, const PaintContext& context) {
    const auto& data = node.paint_data();
    if (!data || data->kind != PaintKind::Text) {
        return;
    }
    // Empty text normally paints nothing, but an active IME preedit still renders at the caret.
    if (data->text.empty() && !(data->decoration && data->decoration->composition_active)) {
        return;
    }
    if (rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    const float opacity = detail::opacity_value(style);
    if (opacity <= 0.0f) {
        return;
    }

    text::TextStyle text_style = text::text_style_from(style);
    if (data->text_style.font_size > 0.0f) {
        text_style.font_size = data->text_style.font_size;
    }
    if (data->text_style.font_weight > 0) {
        text_style.font_weight = data->text_style.font_weight;
    }

    const float padding_left = detail::length_value(style, "padding-left").value_or(0.0f);
    const float padding_right = detail::length_value(style, "padding-right").value_or(0.0f);
    const float padding_top = detail::length_value(style, "padding-top").value_or(0.0f);
    const float padding_bottom = detail::length_value(style, "padding-bottom").value_or(0.0f);
    const float content_width = std::max(0.0f, rect.width - padding_left - padding_right);
    const float content_height = std::max(0.0f, rect.height - padding_top - padding_bottom);
    const float content_x = rect.x + padding_left;
    const float content_y = rect.y + padding_top;

    rhi::Color color = data->text_style.color.value_or(detail::current_color(style));
    color.a *= opacity;
    if (content_width <= 0.0f || content_height <= 0.0f || color.a <= 0.0f ||
        text_style.font_size <= 0.0f) {
        return;
    }

    // `wrap` is authoritative (NoWrap is the V1 default); a computed `white-space` mode that does
    // not wrap (nowrap/pre) can only suppress wrapping, never opt into it. pre-wrap and
    // break-spaces wrap when PaintData asks for it.
    const bool wrap =
        data->wrap == PaintWrap::Wrap && text::white_space_wraps(text_style.white_space);
    StubPaintTextMetrics stub_source;
    const PaintTextMetricsSource* metrics_source =
        context.text_metrics_source != nullptr ? context.text_metrics_source : &stub_source;
    if (data->decoration) {
        paint_text_decoration(*data, {content_x, content_y, content_width, content_height},
                              text_style, color, opacity, wrap,
                              data->text_align.value_or(computed_text_align(style)), sink, stats,
                              context);
        return;
    }
    if (!wrap) {
        // One line. The fast path below (whole text painted as one run) stays the pre-transform
        // single draw, so ordinary text keeps the box rect and sink top-alignment byte-identical.
        // A collapsing mode that trims edges or collapses an interior run, or bidi text that needs
        // the line's visual runs, emits the painted segment sequence instead.
        text::TextConstraints no_wrap_constraints;
        no_wrap_constraints.wrap = false;
        no_wrap_constraints.max_width = content_width;
        text::TextLayout no_wrap_layout =
            metrics_source->layout(data->text, text_style, no_wrap_constraints);
        if (data->max_lines > 0 && no_wrap_layout.lines.size() > data->max_lines) {
            no_wrap_layout.lines.resize(data->max_lines);
        }
        if (no_wrap_layout.lines.empty()) {
            return;
        }
        const text::TextLine& line = no_wrap_layout.lines.front();
        bool identity = false;
        if (line.visual_runs.empty()) {
            text::PaintedSegmentCursor cursor(data->text, line.begin, line.end,
                                              text_style.white_space, true);
            text::PaintedSegment only;
            text::PaintedSegment extra;
            identity = cursor.next(only) && !cursor.next(extra) && only.source_begin == 0 &&
                       only.source_end == data->text.size();
        }
        if (!identity) {
            float line_x = content_x;
            const PaintTextAlign line_align = data->text_align.value_or(computed_text_align(style));
            if (line_align == PaintTextAlign::Center) {
                line_x += (content_width - line.width) * 0.5f;
            } else if (line_align == PaintTextAlign::End) {
                line_x += content_width - line.width;
            }
            emit_line_runs(line, data->text, text_style, *metrics_source,
                           std::max(content_x, line_x), content_y, color, sink, stats);
            return;
        }
        TextDraw run;
        run.rect = {content_x, content_y, content_width, content_height};
        run.text = data->text;
        run.font_size = text_style.font_size;
        run.font_weight = text_style.font_weight;
        run.color = color;
        sink.draw_text(run);
        ++stats.texts;
        return;
    }

    text::TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = content_width;

    // Caller-owned cache: a hit returns stable line boxes without recomputing or allocating; a
    // miss computes exactly what the uncached path would and stores the (already max_lines
    // truncated) result. Null cache keeps the uncached behavior.
    text::TextLayout local_layout;
    const text::TextLayout* layout = nullptr;
    if (context.text_layout_cache != nullptr && context.text_layout_cache->enabled()) {
        layout = &context.text_layout_cache->get(data->text, text_style, constraints,
                                                 *metrics_source, data->max_lines);
    } else {
        local_layout = metrics_source->layout(data->text, text_style, constraints);
        if (data->max_lines > 0 && local_layout.lines.size() > data->max_lines) {
            local_layout.lines.resize(data->max_lines);
        }
        layout = &local_layout;
    }

    const PaintTextAlign align = data->text_align.value_or(computed_text_align(style));
    float line_top = content_y;
    for (const text::TextLine& line : layout->lines) {
        float line_x = content_x;
        if (align == PaintTextAlign::Center) {
            line_x += (content_width - line.width) * 0.5f;
        } else if (align == PaintTextAlign::End) {
            line_x += content_width - line.width;
        }
        emit_line_runs(line, data->text, text_style, *metrics_source, std::max(content_x, line_x),
                       line_top, color, sink, stats);
        line_top += line.height;
    }
}

} // namespace tessera::ui::paint::detail
