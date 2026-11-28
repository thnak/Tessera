// SPDX-License-Identifier: MIT

#include "paint_list_marker.hpp"
#include "paint_properties.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/list_style.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace tessera::ui::paint::detail {
namespace {

constexpr const char* kListItemRole = "listitem";

// Geometry in em of the shape markers, measured against Chrome's default `disc` at 16px: a 5px
// shape whose right edge sits 0.55em before the item and whose centre is 0.35em above the baseline.
constexpr float kShapeEm = 0.3125f;
constexpr float kShapeGapEm = 0.55f;
constexpr float kShapeRiseEm = 0.35f;

const std::string_view* aria_value(const Node& node, std::string_view name) {
    for (const Node::AriaAttribute& aria : node.aria_attributes()) {
        if (aria.name == name) {
            return &aria.value;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> aria_integer(const Node& node, std::string_view name) {
    const std::string_view* text = aria_value(node, name);
    if (text == nullptr) {
        return std::nullopt;
    }
    std::int32_t value = 0;
    const char* end = text->data() + text->size();
    const auto [stop, error] = std::from_chars(text->data(), end, value);
    if (error != std::errc{} || stop != end) {
        return std::nullopt;
    }
    return value;
}

// HTML boolean attribute: present and not "false".
bool aria_flag(const Node& node, std::string_view name) {
    const std::string_view* text = aria_value(node, name);
    return text != nullptr && *text != "false";
}

} // namespace

ListNumber list_item_number(const Node& node) {
    ListNumber result;
    if (node.role() != kListItemRole) {
        return result;
    }
    result.item = true;
    const Node* parent = node.parent();
    if (parent == nullptr) {
        result.value = 1;
        return result;
    }
    std::int64_t count = 0;
    for (const auto& sibling : parent->children()) {
        count += sibling->role() == kListItemRole ? 1 : 0;
    }
    const bool reversed = aria_flag(*parent, "aria-reversed");
    std::int64_t counter = aria_integer(*parent, "aria-start").value_or(reversed ? count : 1);
    for (const auto& sibling : parent->children()) {
        if (sibling->role() != kListItemRole) {
            continue;
        }
        counter = aria_integer(*sibling, "aria-value").value_or(counter);
        if (sibling.get() == &node) {
            break;
        }
        counter += reversed ? -1 : 1;
    }
    result.value = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(counter, std::numeric_limits<std::int32_t>::min(),
                                 std::numeric_limits<std::int32_t>::max()));
    return result;
}

void paint_list_marker(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                       float opacity, const PaintContext& context, PaintSink& sink,
                       PaintStats& stats) {
    const ListNumber number = list_item_number(node);
    const std::string* type_value = style.find("list-style-type");
    if (!number.item || type_value == nullptr || opacity <= 0.0f) {
        return;
    }
    const std::optional<style::ListStyleType> type = style::parse_list_style_type(*type_value);
    if (!type || *type == style::ListStyleType::None) {
        return;
    }
    rhi::Color color = current_color(style);
    color.a *= opacity;
    if (color.a <= 0.0f) {
        return;
    }

    const text::TextStyle text_style = text::text_style_from(style);
    StubPaintTextMetrics stub_source;
    const PaintTextMetricsSource* metrics_source =
        context.text_metrics_source != nullptr ? context.text_metrics_source : &stub_source;
    text::TextConstraints constraints;
    constraints.wrap = false;
    const text::TextLayout first = metrics_source->layout("0", text_style, constraints);
    if (first.lines.empty()) {
        return;
    }
    const text::TextLine& line = first.lines.front();

    const std::optional<rhi::Rect> padding_box = detail::padding_box(style, rect);
    const float line_top =
        (padding_box ? padding_box->y : rect.y) + length_value(style, "padding-top").value_or(0.0f);
    const float baseline = line_top + line.baseline;
    const float size = text_style.font_size;

    if (style::is_bullet_marker(*type)) {
        const float shape = std::max(4.0f, std::round(size * kShapeEm));
        const float right = rect.x - size * kShapeGapEm;
        const rhi::Rect box{std::round(right - shape),
                            std::round(baseline - size * kShapeRiseEm - shape * 0.5f), shape,
                            shape};
        switch (*type) {
        case style::ListStyleType::Disc:
            sink.fill_rounded_rect(box, color, shape * 0.5f);
            break;
        case style::ListStyleType::Circle:
            sink.fill_rounded_ring(box, color, shape * 0.5f,
                                   std::max(1.0f, std::round(size / 16.0f)));
            break;
        case style::ListStyleType::Square:
            sink.fill_rect(box, color);
            break;
        case style::ListStyleType::None:
        case style::ListStyleType::Decimal:
        case style::ListStyleType::LowerAlpha:
        case style::ListStyleType::UpperAlpha:
        case style::ListStyleType::LowerRoman:
        case style::ListStyleType::UpperRoman:
            break;
        }
        ++stats.backgrounds;
        return;
    }

    const std::string marker = style::list_marker_text(*type, number.value);
    const float advance = metrics_source->advance(marker, text_style);
    TextDraw draw;
    draw.rect = {std::round(rect.x - advance), std::round(line_top), advance, line.height};
    draw.text = marker;
    draw.font_size = size;
    draw.font_weight = text_style.font_weight;
    draw.italic = text_style.italic;
    draw.features = text_style.features;
    draw.color = color;
    draw.baseline = std::round(baseline);
    sink.draw_text(draw);
    ++stats.texts;
}

} // namespace tessera::ui::paint::detail
