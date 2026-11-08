// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

std::size_t codepoint_count(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

std::size_t byte_offset(std::string_view text, std::size_t codepoints) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < text.size() && count < codepoints) {
        ++offset;
        while (offset < text.size() &&
               (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u) {
            ++offset;
        }
        ++count;
    }
    return offset;
}

std::vector<std::string_view> split_words(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t start = std::string_view::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            if (start != std::string_view::npos) {
                words.push_back(text.substr(start, i - start));
                start = std::string_view::npos;
            }
        } else if (start == std::string_view::npos) {
            start = i;
        }
    }
    if (start != std::string_view::npos) {
        words.push_back(text.substr(start));
    }
    return words;
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

TextLayout layout_text(std::string_view text, const TextStyle& style, const FontMetrics& metrics,
                       const TextConstraints& constraints) {
    TextLayout result;
    if (text.empty()) {
        return result;
    }

    const float advance = std::max(0.1f, style.font_size * 0.5f + style.letter_spacing);
    const float line_height = style.line_height > 0.0f
                                  ? style.line_height
                                  : metrics.ascent + metrics.descent + metrics.line_gap;

    auto add_line = [&](float width) {
        result.lines.push_back(TextLine{width, line_height, metrics.ascent});
        result.width = std::max(result.width, width);
    };

    if (!constraints.wrap || style.nowrap || !std::isfinite(constraints.max_width)) {
        add_line(static_cast<float>(codepoint_count(text)) * advance);
        result.height = line_height;
        result.baseline = metrics.ascent;
        return result;
    }

    const float max_width = std::max(advance, constraints.max_width);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view paragraph = text.substr(start, end - start);

        float current = 0.0f;
        bool has_line = false;
        for (const std::string_view word : split_words(paragraph)) {
            const float word_width = static_cast<float>(codepoint_count(word)) * advance;
            if (word_width > max_width) {
                if (has_line) {
                    add_line(current);
                    current = 0.0f;
                    has_line = false;
                }
                std::string_view remaining = word;
                while (!remaining.empty()) {
                    const std::size_t max_codepoints =
                        std::max<std::size_t>(1, static_cast<std::size_t>(max_width / advance));
                    const std::size_t take = std::min(max_codepoints, codepoint_count(remaining));
                    add_line(static_cast<float>(take) * advance);
                    remaining.remove_prefix(byte_offset(remaining, take));
                }
                continue;
            }

            if (!has_line) {
                current = word_width;
                has_line = true;
            } else if (current + advance + word_width <= max_width) {
                current += advance + word_width;
            } else {
                add_line(current);
                current = word_width;
                has_line = true;
            }
        }
        if (has_line) {
            add_line(current);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }

    if (result.lines.empty()) {
        add_line(0.0f);
    }
    result.height = static_cast<float>(result.lines.size()) * line_height;
    result.baseline = metrics.ascent;
    return result;
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

} // namespace tessera::ui::text
