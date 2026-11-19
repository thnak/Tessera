// SPDX-License-Identifier: MIT

#include "paint_properties.hpp"
#include "paint_text_rules.hpp"
#include "tessera/ui/style/text_decoration.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

namespace tessera::ui::paint::detail {
namespace {

// 1/16 em at the default size is one pixel; larger text gets proportionally thicker rules.
constexpr float kAutoThicknessEm = 1.0f / 16.0f;
// The underline sits this fraction of the descent below the baseline (at least one pixel); the
// line-through crosses this many ems above it; the overline touches the ascent.
constexpr float kUnderlineDescentFraction = 0.4f;
constexpr float kStrikeHeightEm = 0.3f;

bool is_current_color(const std::string& value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("currentcolor") != std::string::npos;
}

} // namespace

TextRuleStyle resolve_text_rules(const style::ComputedStyle& computed,
                                 const text::TextStyle& text_style, const rhi::Color& text_color,
                                 float opacity) {
    TextRuleStyle rules;
    rules.lines = text_style.decoration_lines;
    rules.thickness = text_style.decoration_thickness;
    rules.color = text_color;
    if (rules.lines == 0) {
        return rules;
    }
    const std::string* declared = computed.find("text-decoration-color");
    if (declared == nullptr || is_current_color(*declared)) {
        return rules;
    }
    if (const std::optional<rhi::Color> explicit_color =
            color_value(computed, "text-decoration-color", text_color)) {
        rules.color = *explicit_color;
        rules.color.a *= opacity;
    }
    return rules;
}

void emit_text_rules(const TextRuleStyle& rules, const text::FontMetrics& metrics, float font_size,
                     float left, float right, float baseline_y, PaintSink& sink) {
    if (rules.lines == 0 || rules.color.a <= 0.0f) {
        return;
    }
    const float x0 = std::round(left);
    const float x1 = std::round(right);
    if (x1 <= x0) {
        return;
    }
    const float thickness = rules.thickness > 0.0f
                                ? rules.thickness
                                : std::max(1.0f, std::round(font_size * kAutoThicknessEm));
    const auto draw = [&](float top) {
        sink.fill_rect({x0, std::round(top), x1 - x0, thickness}, rules.color);
    };
    if ((rules.lines & style::kDecorationOverline) != 0) {
        draw(baseline_y - metrics.ascent);
    }
    if ((rules.lines & style::kDecorationLineThrough) != 0) {
        draw(baseline_y - font_size * kStrikeHeightEm - thickness * 0.5f);
    }
    if ((rules.lines & style::kDecorationUnderline) != 0) {
        draw(baseline_y + std::max(1.0f, metrics.descent * kUnderlineDescentFraction));
    }
}

} // namespace tessera::ui::paint::detail
