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

// CSS `white-space` keyword to the mode the wrap understands (docs/design/text.md section 6).
// `nowrap` keeps one line but collapses runs, `pre` keeps one literal line, and `pre-line` maps to
// Normal because the paragraph split already honors hard newlines. Unknown values stay tolerant
// like the rest of the parser.
WhiteSpace parse_white_space(std::string_view value) {
    const std::string token = lower(trim(value));
    if (token == "nowrap") {
        return WhiteSpace::NoWrap;
    }
    if (token == "pre") {
        return WhiteSpace::Pre;
    }
    if (token == "pre-wrap") {
        return WhiteSpace::PreWrap;
    }
    if (token == "break-spaces") {
        return WhiteSpace::BreakSpaces;
    }
    return WhiteSpace::Normal;
}

// CSS `text-overflow` (docs/design/text.md section 6.3). The registry rejects the two-value
// `<overflow> <side>` form, so a computed value is one keyword (or a keyword the parser did not
// understand, which stays tolerant like `white-space`). Only `ellipsis` opts into paint-time
// truncation; everything else is `clip`.
TextOverflow parse_text_overflow(std::string_view value) {
    return lower(trim(value)) == "ellipsis" ? TextOverflow::Ellipsis : TextOverflow::Clip;
}

// CSS `text-transform` keyword to the V1 ASCII mapping (docs/design/text.md section 6.5). `none`,
// an unsupported keyword (`full-width`, `math-auto`, ...) and garbage all stay `None`, like the
// other tolerant computed-value readers here.
TextTransform parse_text_transform(std::string_view value) {
    const std::string token = lower(trim(value));
    if (token == "uppercase") {
        return TextTransform::Uppercase;
    }
    if (token == "lowercase") {
        return TextTransform::Lowercase;
    }
    if (token == "capitalize") {
        return TextTransform::Capitalize;
    }
    return TextTransform::None;
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

std::size_t word_separator_count(std::string_view run) noexcept {
    std::size_t count = 0;
    for (const char c : run) {
        // V1 widens the ASCII space only. A tab is not a word separator (CSS leaves its spacing
        // undefined), a hard line break splits the paragraph, and a non-ASCII separator keeps its
        // font advance.
        if (c == ' ') {
            ++count;
        }
    }
    return count;
}

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
    if (const std::string* spacing = computed.find("word-spacing")) {
        // CSS `normal`, an unsupported keyword and a negative length all mean "no extra advance".
        // The computed font size is the base for the font-relative forms, like `letter-spacing`.
        const float parsed = parse_length(*spacing, result.font_size);
        if (parsed > 0.0f) {
            result.word_spacing = parsed;
        }
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
        result.white_space = parse_white_space(*white_space);
    }
    if (const std::string* transform = computed.find("text-transform")) {
        result.text_transform = parse_text_transform(*transform);
    }
    if (const std::string* overflow = computed.find("text-overflow")) {
        result.text_overflow = parse_text_overflow(*overflow);
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
