// SPDX-License-Identifier: MIT
//
// Attributed paint runs for a Text node with inline spans (issue #229, elements-basic.md section
// 2.1). The parent measures/wraps one paragraph with its own style (V1 spans may not change
// metrics); this file splits each laid-out line's painted segments at the span byte boundaries and
// emits one draw per slice with that span's resolved weight/italic/colour. It also stores each
// span's run rectangle on the span's child node, which is what the generic hit test uses to resolve
// a point to the span (the parent Text is the only layout box).

#include "paint_properties.hpp"
#include "paint_text.hpp"
#include "paint_text_rules.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/text_decoration.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/size_runs.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::paint::detail {
namespace {

struct SpanRunStyle {
    std::size_t begin = 0;
    std::size_t end = 0;
    int font_weight = 0;
    bool italic = false;
    rhi::Color color{0.0f, 0.0f, 0.0f, 1.0f};
    // The span's own `text-decoration` (inherited from the parent through the cascade) and its
    // `vertical-align` baseline shift in px, positive raising the run (not inherited).
    TextRuleStyle rules;
    float shift = 0.0f;
    // The span's font size over the paragraph's (1 = the paragraph's own size; below 1 for
    // `sub`/`sup`/`small`). Mirrors the table layout measured, see text::size_runs_from_spans.
    float scale = 1.0f;
    float font_size = 0.0f;
    // The span's own `background-color` (not inherited; `mark`), transparent when unset. It fills
    // the line box of each slice, so adjacent slices of one span abut.
    rhi::Color background{0.0f, 0.0f, 0.0f, 0.0f};
    // Union of the painted rectangles of this span, in absolute paint coordinates; `painted` is
    // false until the first slice is emitted.
    rhi::Rect rect;
    bool painted = false;
};

bool identity_transform(const rhi::Mat3& matrix) {
    const rhi::Mat3 identity;
    for (int index = 0; index < 9; ++index) {
        if (matrix.m[index] != identity.m[index]) {
            return false;
        }
    }
    return true;
}

const Node* find_child(const Node& node, std::uint64_t id) {
    for (const auto& child : node.children()) {
        if (child->id() == id) {
            return child.get();
        }
    }
    return nullptr;
}

// The span covering `offset`, or kMaxPaintSpans for an unattributed byte.
std::size_t span_index_at(const std::vector<PaintSpan>& spans, std::size_t offset) {
    for (std::size_t index = 0; index < spans.size(); ++index) {
        if (offset >= spans[index].begin && offset < spans[index].end) {
            return index;
        }
    }
    return spans.size();
}

void union_rect(SpanRunStyle& style, const rhi::Rect& rect) {
    if (!style.painted) {
        style.rect = rect;
        style.painted = true;
        return;
    }
    const float left = std::min(style.rect.x, rect.x);
    const float top = std::min(style.rect.y, rect.y);
    const float right = std::max(style.rect.x + style.rect.width, rect.x + rect.width);
    const float bottom = std::max(style.rect.y + style.rect.height, rect.y + rect.height);
    style.rect = rhi::Rect{left, top, right - left, bottom - top};
}

} // namespace

void paint_spanned_text(const Node& node, const text::TextStyle& base_style,
                        const rhi::Rect& content, const rhi::Color& parent_color,
                        const TextRuleStyle& parent_rules, float opacity, bool wrap,
                        PaintTextAlign align, std::uint32_t max_lines, const PaintContext& context,
                        PaintSink& sink, PaintStats& stats) {
    const auto& data = node.paint_data();
    if (!data || data->kind != PaintKind::Text || data->spans.empty()) {
        return;
    }

    // Resolve the run style of every span through the normal cascade. A span's color inherits from
    // the parent, so a span without a color declaration keeps painting the parent color; its metric
    // fields are deliberately ignored (the spec rule that keeps the parent's measurement exact).
    SpanRunStyle styles[kMaxPaintSpans];
    const std::size_t span_count = std::min(data->spans.size(), kMaxPaintSpans);
    // The size the layout pass measured the paragraph at: the node's computed font size, not a
    // PaintData override, so both passes derive the same span ratios.
    float computed_size = base_style.font_size;
    if (context.styles != nullptr) {
        computed_size = text::text_style_from(context.styles->compute(node)).font_size;
    }
    std::vector<text::SpanFontSize> sizes;
    for (std::size_t index = 0; index < span_count; ++index) {
        const PaintSpan& span = data->spans[index];
        SpanRunStyle& run = styles[index];
        run.font_size = base_style.font_size;
        run.begin = span.begin;
        run.end = span.end;
        run.font_weight = base_style.font_weight;
        run.italic = base_style.italic;
        run.color = parent_color;
        run.rules = parent_rules;
        if (context.styles == nullptr) {
            continue;
        }
        const Node* child = find_child(node, span.node_id);
        if (child == nullptr) {
            continue;
        }
        const style::ComputedStyle span_style = context.styles->compute(*child);
        const text::TextStyle run_text = text::text_style_from(span_style);
        run.font_weight = run_text.font_weight;
        run.italic = run_text.italic;
        sizes.push_back({span.begin, span.end, run_text.font_size});
        if (computed_size > 0.0f && run_text.font_size > 0.0f &&
            run_text.font_size < computed_size) {
            run.scale = std::max(text::kMinTextSizeRunScale, run_text.font_size / computed_size);
            run.font_size = base_style.font_size * run.scale;
        }
        run.color = detail::current_color(span_style);
        run.color.a *= opacity;
        run.rules = resolve_text_rules(span_style, run_text, run.color, opacity);
        if (const std::optional<rhi::Color> fill =
                detail::color_value(span_style, "background-color", run.color)) {
            run.background = *fill;
            run.background.a *= opacity;
        }
        // The em-relative shift is taken against the paragraph's size, not the span's own.
        if (const std::string* shift_value = span_style.find("vertical-align")) {
            run.shift = style::vertical_align_shift(*shift_value, base_style.font_size);
        }
    }

    StubPaintTextMetrics stub_source;
    const PaintTextMetricsSource* metrics_source =
        context.text_metrics_source != nullptr ? context.text_metrics_source : &stub_source;
    text::TextConstraints constraints;
    constraints.wrap = wrap;
    constraints.max_width = content.width;
    text::TextStyle layout_style = base_style;
    layout_style.size_runs = text::size_runs_from_spans(sizes, computed_size);
    text::TextLayout layout = metrics_source->layout(data->text, layout_style, constraints);
    if (max_lines > 0 && layout.lines.size() > max_lines) {
        layout.lines.resize(max_lines);
    }

    float line_top = content.y;
    for (const text::TextLine& line : layout.lines) {
        float line_x = content.x;
        if (align == PaintTextAlign::Center) {
            line_x += (content.width - line.width) * 0.5f;
        } else if (align == PaintTextAlign::End) {
            line_x += content.width - line.width;
        }
        line_x = std::max(content.x, line_x);

        float x = line_x + line.indent;
        // The layout already trims the line edges; the cursor is asked to trim only at the real
        // line edges so a collapsed run stays one space.
        text::PaintedSegmentCursor cursor(data->text, line.begin, line.end, base_style.white_space,
                                          true);
        text::PaintedSegment segment;
        while (cursor.next(segment)) {
            const auto emit_slice = [&](std::string_view painted, std::size_t source_begin,
                                        float width, bool collapsed) {
                const std::size_t index = span_index_at(data->spans, source_begin);
                const SpanRunStyle& span_style =
                    index < span_count ? styles[index] : SpanRunStyle{};
                TextDraw draw;
                const float shift = index < span_count ? span_style.shift : 0.0f;
                const TextRuleStyle& rules = index < span_count ? span_style.rules : parent_rules;
                draw.rect = {std::round(x), std::round(line_top - shift), width, line.height};
                draw.text = painted;
                draw.font_size =
                    index < span_count && !collapsed ? span_style.font_size : base_style.font_size;
                draw.font_weight =
                    index < span_count ? span_style.font_weight : base_style.font_weight;
                draw.italic = index < span_count ? span_style.italic : base_style.italic;
                draw.features = base_style.features;
                draw.color = index < span_count ? span_style.color : parent_color;
                draw.baseline = std::round(line_top + line.baseline - shift);
                if (index < span_count) {
                    union_rect(styles[index], draw.rect);
                }
                if (index < span_count && span_style.background.a > 0.0f && width > 0.0f) {
                    sink.fill_rect({std::round(x), std::round(line_top), width, line.height},
                                   span_style.background);
                }
                if (!painted.empty() && draw.color.a > 0.0f) {
                    sink.draw_text(draw);
                    ++stats.texts;
                    emit_text_rules(rules, metrics_source->metrics(base_style),
                                    base_style.font_size, x, x + width,
                                    line_top + line.baseline - shift, sink);
                }
                x += width;
            };
            if (segment.collapsed) {
                // A collapsed whitespace run paints as one space; splitting it would change the
                // measured advance, so it takes the style of the byte that starts it.
                emit_slice(segment.source, segment.source_begin,
                           metrics_source->advance(segment.source, base_style), true);
                continue;
            }
            std::size_t position = segment.source_begin;
            while (position < segment.source_end) {
                const std::size_t index = span_index_at(data->spans, position);
                std::size_t slice_end = segment.source_end;
                if (index < span_count) {
                    slice_end = std::min(slice_end, data->spans[index].end);
                } else {
                    // Unattributed bytes end at the next span start (or the segment end).
                    for (const PaintSpan& span : data->spans) {
                        if (span.begin > position) {
                            slice_end = std::min(slice_end, span.begin);
                            break;
                        }
                    }
                }
                if (slice_end <= position) {
                    slice_end = segment.source_end;
                }
                const std::string_view slice =
                    std::string_view(data->text).substr(position, slice_end - position);
                std::string transformed;
                std::string_view painted = slice;
                if (base_style.text_transform != text::TextTransform::None) {
                    transformed = text::apply_text_transform(slice, base_style.text_transform);
                    painted = transformed;
                }
                text::TextStyle slice_style = base_style;
                if (index < span_count) {
                    slice_style.font_size = styles[index].font_size;
                }
                emit_slice(painted, position, metrics_source->advance(slice, slice_style), false);
                position = slice_end;
            }
        }
        line_top += line.height;
    }

    // The hit test resolves a point to the span through these rectangles. Bounds are
    // parent-relative; a transformed Text keeps only parent-level hit testing in V1
    // (elements-basic.md section 2.1), so a non-identity transform skips storing them.
    if (!identity_transform(node.transform())) {
        return;
    }
    // Only a span that is interactive or semantic becomes a hit target / accessibility entry: the
    // plain synthetic runs stay part of the parent Text (elements-basic.md section 2.1).
    // `set_span_bounds` is const (issue #733 item 4): the pass is handed the tree as `const` and
    // records paint-derived data through it, so no `const_cast` is needed to reach a child.
    const rhi::Rect world = node.world_bounds();
    for (std::size_t index = 0; index < span_count; ++index) {
        if (!styles[index].painted) {
            continue;
        }
        for (const auto& child : node.children()) {
            if (child->id() != data->spans[index].node_id) {
                continue;
            }
            if (child->has_event_binding(EventType::Click) || !child->role().empty()) {
                child->set_span_bounds(
                    rhi::Rect{styles[index].rect.x - world.x, styles[index].rect.y - world.y,
                              styles[index].rect.width, styles[index].rect.height});
            }
            break;
        }
    }
}

} // namespace tessera::ui::paint::detail
