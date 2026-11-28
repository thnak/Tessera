// SPDX-License-Identifier: MIT
//
// PaintData text payload emission: single-run, wrapped and bidi glyph runs, plus the caret,
// selection and IME preedit decoration. Paint consumes byte offsets and resolves them through the
// same line boxes the layout produced; it never shapes or lays out text itself.

#include "lcd_text_sink.hpp"
#include "paint_list_marker.hpp"
#include "paint_properties.hpp"
#include "paint_text.hpp"
#include "paint_text_flex.hpp"
#include "paint_text_rules.hpp"
#include "paint_text_runs.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::paint::detail {
namespace {

// The advance seam the inside marker measures its counter with: paint's own metrics source.
float marker_advance(std::string_view text, const text::TextStyle& style, const void* user) {
    return static_cast<const PaintTextMetricsSource*>(user)->advance(text, style);
}

// Paints an optional PaintData text run (D9) after the node's background/borders and inside the
// active clip. Precedence: PaintData override > computed style > backend default; node opacity
// multiplies the effective color alpha. NoWrap (default) emits one run; Wrap uses the shared
// text-line-box layout (`text::layout_text`) with the content box width and emits one run per line.
// Caret/selection emission. Paint only consumes byte offsets: it resolves them through the same
// line boxes the wrap path uses. Z-order: selection behind the glyph runs, caret after them. The
// caret is the text color at full alpha, 1 px at device scale 1 and 2 px at >= 2; a blink phase
// below 0.5 hides only the caret, because paint has no timers.
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
            // run-local visual intervals; selection uses run-local affinity (an RTL run's logical
            // start is its visual right edge), unlike the caret's global join affinity.
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
    // Caret/selection/preedit paint the laid-out line verbatim: truncating under an editing caret
    // would desynchronize the byte offsets from what is drawn, so `text-overflow` is clip here.
    for (const LineGeometry& geometry : lines) {
        emit_line_runs(geometry.line, text, text_style, *metrics_source, geometry.x, geometry.top,
                       std::numeric_limits<float>::infinity(), color, sink, stats);
    }

    // IME preedit: underline the composition range and, when the preedit is not part of the painted
    // text, draw the uncommitted run at the composition anchor: after the glyph runs (above the
    // selection), before the caret. The byte range -> line-box mapping is the selection/caret one.
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
            run.features = text_style.features;
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
        // The shared helper, so the IME's `requestCursorUpdates` answer is this geometry exactly.
        const std::size_t caret = std::min(decoration.caret_byte_offset, text.size());
        text::TextLayout layout;
        layout.lines.reserve(lines.size());
        // A line break matches both ranges; the FIRST is where the caret was typed.
        const LineGeometry* caret_geometry = &lines.back();
        bool found = false;
        for (const LineGeometry& geometry : lines) {
            layout.lines.push_back(geometry.line);
            if (!found && caret >= geometry.line.begin && caret <= geometry.line.end) {
                caret_geometry = &geometry;
                found = true;
            }
        }
        const CaretBox box =
            caret_box(text, layout, decoration.caret_byte_offset, text_style, *metrics_source,
                      context.device_scale, caret_geometry->x, caret_geometry->top);
        sink.fill_rect(
            {std::round(box.x), std::round(box.y), std::max(box.width, 1.0f), box.height}, color);
    }
}

} // namespace

void paint_text_lines(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                      float opacity, PaintSink& sink, PaintStats& stats,
                      const PaintContext& context, bool lcd) {
    if (lcd) {
        // Tag every run this node emits once, instead of at each of the seven TextDraw sites.
        LcdTextSink tagged(sink);
        paint_text_lines(node, style, rect, opacity, tagged, stats, context, false);
        return;
    }
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
    float content_height = std::max(0.0f, rect.height - padding_top - padding_bottom);
    const float content_x = rect.x + padding_left;
    float content_y = rect.y + padding_top;

    rhi::Color color = data->text_style.color.value_or(detail::current_color(style));
    color.a *= opacity;
    if (content_width <= 0.0f || content_height <= 0.0f || color.a <= 0.0f ||
        text_style.font_size <= 0.0f) {
        return;
    }

    StubPaintTextMetrics stub_source;
    const PaintTextMetricsSource* metrics_source =
        context.text_metrics_source != nullptr ? context.text_metrics_source : &stub_source;
    // `list-style-position: inside`: the first Text of the item reserves the marker's box at the
    // start of its first line (docs/design/list-style-inside.md) before anything is measured.
    std::optional<InsideMarker> inside_marker_box;
    if (context.styles != nullptr && !data->decoration) {
        inside_marker_box = inside_marker(node, *context.styles, &marker_advance, metrics_source);
        if (inside_marker_box) {
            text_style.first_line_indent = inside_marker_box->width;
        }
    }
    // A Text wraps, as in a browser, unless `white-space` forbids it or layout gave it one line.
    const bool wrap = text_wraps(node.element_kind(), *data, text_style.white_space,
                                 *metrics_source, text_style, content_height);
    const FlexTextCentering flex = flex_text_centering(node.element_kind(), style);
    PaintTextAlign text_align = data->text_align.value_or(computed_text_align(style));
    text_align = flex.horizontal ? PaintTextAlign::Center : text_align;
    if (((node.element_kind() == "TextInput" || node.element_kind() == "Button") && !wrap) ||
        flex.vertical) {
        // A single-line field and a button's caption centre their line in the content box, as a
        // browser does (the caption is the node's own text, so no flex item exists for
        // `align-items` to centre, issue #602); so does a flex `Text` leaf (`flex`). A `TextArea`
        // and ordinary `Text` stay top-aligned. Caret and selection use the same content box.
        text::TextConstraints one_line;
        one_line.wrap = wrap;
        one_line.max_width = content_width;
        const text::TextLayout probe = metrics_source->layout(
            data->text.empty() ? std::string("x") : data->text, text_style, one_line);
        if (!probe.lines.empty()) {
            float lines_height = 0.0f;
            for (const text::TextLine& line : probe.lines) {
                lines_height += line.height;
            }
            const float offset = std::max(0.0f, (content_height - lines_height) * 0.5f);
            content_y += offset;
            content_height = std::max(0.0f, content_height - offset);
        }
    }
    // `vertical-align` is a paint-time baseline shift (docs/design/text-decoration.md): the line
    // box, the node's size and every layout cache stay as `baseline` produces them, and the text
    // and its rules move together.
    content_y -= text_style.baseline_shift;
    if (inside_marker_box) {
        paint_inside_marker(*inside_marker_box, data->text, text_style, *metrics_source, wrap,
                            {content_x, content_y, content_width, content_height}, text_align,
                            opacity, sink, stats);
    }
    const TextRuleStyle rules = resolve_text_rules(style, text_style, color, opacity);
    const auto paint_rules = [&](const text::TextLine& line, float left, float available,
                                 float line_top) {
        if (rules.lines != 0) {
            // The first-line box is not text: no underline runs through a list marker's room.
            emit_text_rules(rules, metrics_source->metrics(text_style), text_style.font_size,
                            left + line.indent, left + std::min(line.width, available),
                            line_top + line.baseline, sink);
        }
    };
    if (!data->spans.empty() && !data->decoration) {
        // Inline spans: one measured paragraph, per-span painted runs (issue #229).
        paint_spanned_text(node, text_style, {content_x, content_y, content_width, content_height},
                           color, rules, opacity, wrap, text_align, data->max_lines, context, sink,
                           stats);
        return;
    }
    if (data->decoration) {
        paint_text_decoration(*data, {content_x, content_y, content_width, content_height},
                              text_style, color, opacity, wrap, text_align, sink, stats, context);
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
        // Alignment first: the ellipsis fit is what the line has left from its start offset, so an
        // over-wide line (the documented case) starts at the content box and gets its full width.
        float line_x = content_x;
        const PaintTextAlign line_align = text_align;
        if (line_align == PaintTextAlign::Center) {
            line_x += (content_width - line.width) * 0.5f;
        } else if (line_align == PaintTextAlign::End) {
            line_x += content_width - line.width;
        }
        line_x = std::max(content_x, line_x);
        const float available_width = std::max(0.0f, content_x + content_width - line_x);
        const bool truncate = text_style.text_overflow == text::TextOverflow::Ellipsis &&
                              line.width > available_width;
        bool identity = false;
        if (!truncate && line.visual_runs.empty()) {
            text::PaintedSegmentCursor cursor(data->text, line.begin, line.end,
                                              text_style.white_space, true);
            text::PaintedSegment only;
            text::PaintedSegment extra;
            identity = cursor.next(only) && !cursor.next(extra) && only.source_begin == 0 &&
                       only.source_end == data->text.size();
        }
        if (!identity) {
            emit_line_runs(line, data->text, text_style, *metrics_source, line_x, content_y,
                           available_width, color, sink, stats);
            paint_rules(line, line_x, available_width, content_y);
            return;
        }
        TextDraw run;
        // The run starts at the aligned origin, not the content-box origin: the sink draws from
        // `rect.x` as the pen start, so `content_x` here would discard the alignment computed above
        // for every single-line run (issue #313). The width narrows to what is left from there,
        // matching `available_width` of the run-sequence path.
        run.rect = {line_x + line.indent, content_y, std::max(0.0f, available_width - line.indent),
                    content_height};
        // The sink puts the baseline at `rect.y + ascent` unless told otherwise; an explicit
        // `line-height` moves it by the half-leading, so say where the line layout put it.
        const float half_leading = line.baseline - metrics_source->metrics(text_style).ascent;
        if (std::abs(half_leading) > 0.01f) {
            run.baseline = content_y + line.baseline;
        }
        // The whole line paints as one run: emit the transformed bytes, exactly what the measured
        // wrap used, so the painted sequence matches the (transformed) line width.
        std::string transformed;
        std::string_view painted = data->text;
        if (text_style.text_transform != text::TextTransform::None) {
            transformed = text::apply_text_transform(data->text, text_style.text_transform);
            painted = transformed;
        }
        run.text = painted;
        run.font_size = text_style.font_size;
        run.font_weight = text_style.font_weight;
        run.features = text_style.features;
        run.color = color;
        sink.draw_text(run);
        ++stats.texts;
        paint_rules(line, line_x, available_width, content_y);
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

    const PaintTextAlign align = text_align;
    float line_top = content_y;
    for (const text::TextLine& line : layout->lines) {
        float line_x = content_x;
        if (align == PaintTextAlign::Center) {
            line_x += (content_width - line.width) * 0.5f;
        } else if (align == PaintTextAlign::End) {
            line_x += content_width - line.width;
        }
        line_x = std::max(content_x, line_x);
        emit_line_runs(line, data->text, text_style, *metrics_source, line_x, line_top,
                       std::max(0.0f, content_x + content_width - line_x), color, sink, stats);
        paint_rules(line, line_x, std::max(0.0f, content_x + content_width - line_x), line_top);
        line_top += line.height;
    }
}

} // namespace tessera::ui::paint::detail
