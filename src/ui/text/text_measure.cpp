// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "text_wrap.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::ui::text {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool parse_number(std::string_view text, float& value) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

float parse_length(std::string_view value, float relative_to) {
    const std::string token = lower(trim(value));
    if (token.empty() || token == "normal") {
        return 0.0f;
    }
    if (token.back() == '%') {
        float percent = 0.0f;
        if (!parse_number(std::string_view(token).substr(0, token.size() - 1), percent)) {
            return 0.0f;
        }
        return relative_to * percent / 100.0f;
    }
    if (token.size() > 2 && token.substr(token.size() - 2) == "px") {
        float pixels = 0.0f;
        return parse_number(std::string_view(token).substr(0, token.size() - 2), pixels) ? pixels
                                                                                         : 0.0f;
    }
    if (token.size() > 3 && token.substr(token.size() - 3) == "rem") {
        float rems = 0.0f;
        return parse_number(std::string_view(token).substr(0, token.size() - 3), rems)
                   ? rems * relative_to
                   : 0.0f;
    }
    if (token.size() > 2 && token.substr(token.size() - 2) == "em") {
        float ems = 0.0f;
        return parse_number(std::string_view(token).substr(0, token.size() - 2), ems)
                   ? ems * relative_to
                   : 0.0f;
    }
    float number = 0.0f;
    return parse_number(token, number) ? number : 0.0f;
}

} // namespace

TextStyle text_style_from(const style::ComputedStyle& computed) {
    TextStyle result;
    if (const std::string* size = computed.find("font-size")) {
        const float parsed = parse_length(*size, 16.0f);
        if (parsed > 0.0f) {
            result.font_size = parsed;
        }
    }
    if (const std::string* line_height = computed.find("line-height")) {
        const std::string value = lower(trim(*line_height));
        if (value != "normal") {
            if (!value.empty() && value.back() == '%') {
                result.line_height = parse_length(value, result.font_size);
            } else {
                float number = 0.0f;
                if (parse_number(value, number)) {
                    result.line_height = number * result.font_size;
                } else {
                    result.line_height = parse_length(value, result.font_size);
                }
            }
        }
    }
    if (const std::string* spacing = computed.find("letter-spacing")) {
        result.letter_spacing = parse_length(*spacing, result.font_size);
    }
    if (const std::string* weight = computed.find("font-weight")) {
        float number = 0.0f;
        if (parse_number(*weight, number)) {
            result.font_weight = static_cast<int>(number);
        }
    }
    if (const std::string* font_style = computed.find("font-style")) {
        result.italic = lower(trim(*font_style)) == "italic";
    }
    if (const std::string* white_space = computed.find("white-space")) {
        const std::string value = lower(trim(*white_space));
        result.nowrap = value == "nowrap" || value == "pre";
    }
    return result;
}

StubTextMeasurer::StubTextMeasurer(std::shared_ptr<const FontMetricsProvider> metrics_provider)
    : metrics_provider_(metrics_provider != nullptr ? std::move(metrics_provider)
                                                    : std::make_shared<StubFontMetricsProvider>()) {
}

// Line boxes come from the shared greedy wrap in text_wrap.cpp; this entry point only selects the
// deterministic stub advance and the break engine the build carries.
TextLayout layout_text(std::string_view text, const TextStyle& style, const FontMetrics& metrics,
                       const TextConstraints& constraints) {
    return layout_text_with_breaks(text, style, metrics, constraints, active_backend());
}

TextMetrics StubTextMeasurer::measure(std::string_view text, const TextStyle& style,
                                      const TextConstraints& constraints) const {
    if (text.empty()) {
        return {};
    }

    const FontMetrics font_metrics = metrics_provider_->metrics(style);
    const TextLayout layout = layout_text(text, style, font_metrics, constraints);
    return TextMetrics{layout.width, layout.height, layout.baseline,
                       static_cast<int>(layout.lines.size())};
}

TextLayout layout_text_measured(std::string_view text, const TextStyle& style,
                                const FontMetrics& metrics, const TextConstraints& constraints,
                                TextAdvanceMeasure measure, void* user) {
    if (text.empty() || measure == nullptr) {
        return {};
    }
    return layout_text_with_breaks(text, style, metrics, constraints, active_backend(), measure,
                                   user);
}

} // namespace tessera::ui::text
