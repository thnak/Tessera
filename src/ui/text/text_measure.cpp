// SPDX-License-Identifier: MIT

#include "tessera/ui/text/bidi.hpp"
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

TextLayout finalize_bidi(TextLayout layout, std::string_view text) {
    layout.direction = bidi_paragraph_direction(text);
    bidi_apply_visual_runs(text, layout.direction, layout.lines);
    return layout;
}

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

struct WordRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<WordRange> split_word_ranges(std::string_view text, std::size_t begin,
                                         std::size_t end) {
    std::vector<WordRange> words;
    std::size_t word_begin = std::string_view::npos;
    for (std::size_t i = begin; i < end; ++i) {
        if (std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            if (word_begin != std::string_view::npos) {
                words.push_back(WordRange{word_begin, i});
                word_begin = std::string_view::npos;
            }
        } else if (word_begin == std::string_view::npos) {
            word_begin = i;
        }
    }
    if (word_begin != std::string_view::npos) {
        words.push_back(WordRange{word_begin, end});
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

    auto add_line = [&](float width, std::size_t begin, std::size_t end) {
        result.lines.push_back(TextLine{width, line_height, metrics.ascent, begin, end, {}});
        result.width = std::max(result.width, width);
    };

    if (!constraints.wrap || style.nowrap || !std::isfinite(constraints.max_width)) {
        add_line(static_cast<float>(codepoint_count(text)) * advance, 0, text.size());
        result.height = line_height;
        result.baseline = metrics.ascent;
        return finalize_bidi(std::move(result), text);
    }

    const float max_width = std::max(advance, constraints.max_width);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::size_t lines_before = result.lines.size();

        float current = 0.0f;
        bool has_line = false;
        std::size_t line_begin = 0;
        std::size_t line_end = 0;
        for (const WordRange& word : split_word_ranges(text, start, end)) {
            const std::string_view word_text = text.substr(word.begin, word.end - word.begin);
            const float word_width = static_cast<float>(codepoint_count(word_text)) * advance;
            if (word_width > max_width) {
                if (has_line) {
                    add_line(current, line_begin, line_end);
                    current = 0.0f;
                    has_line = false;
                }
                std::string_view remaining = word_text;
                std::size_t chunk_begin = word.begin;
                while (!remaining.empty()) {
                    const std::size_t max_codepoints =
                        std::max<std::size_t>(1, static_cast<std::size_t>(max_width / advance));
                    const std::size_t take = std::min(max_codepoints, codepoint_count(remaining));
                    const std::size_t chunk_end = chunk_begin + byte_offset(remaining, take);
                    add_line(static_cast<float>(take) * advance, chunk_begin, chunk_end);
                    remaining.remove_prefix(chunk_end - chunk_begin);
                    chunk_begin = chunk_end;
                }
                continue;
            }

            if (!has_line) {
                current = word_width;
                line_begin = word.begin;
                line_end = word.end;
                has_line = true;
            } else if (current + advance + word_width <= max_width) {
                current += advance + word_width;
                line_end = word.end;
            } else {
                add_line(current, line_begin, line_end);
                current = word_width;
                line_begin = word.begin;
                line_end = word.end;
                has_line = true;
            }
        }
        if (has_line) {
            add_line(current, line_begin, line_end);
        }
        if (result.lines.size() == lines_before) {
            // A blank (or whitespace-only) paragraph still owns a line box, so a caret after Enter
            // and a trailing newline land on their own line instead of the previous one.
            add_line(0.0f, start, end);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }

    if (result.lines.empty()) {
        add_line(0.0f, 0, text.size());
    }
    result.height = static_cast<float>(result.lines.size()) * line_height;
    result.baseline = metrics.ascent;
    return finalize_bidi(std::move(result), text);
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
    TextLayout result;
    if (text.empty() || measure == nullptr) {
        return result;
    }

    const float line_height = style.line_height > 0.0f
                                  ? style.line_height
                                  : metrics.ascent + metrics.descent + metrics.line_gap;
    const auto measure_text = [&](std::string_view run) {
        return std::max(0.0f, measure(run, style, user));
    };
    const float space_advance = measure_text(" ");

    auto add_line = [&](float width, std::size_t begin, std::size_t end) {
        result.lines.push_back(TextLine{width, line_height, metrics.ascent, begin, end, {}});
        result.width = std::max(result.width, width);
    };

    if (!constraints.wrap || style.nowrap || !std::isfinite(constraints.max_width)) {
        add_line(measure_text(text), 0, text.size());
        result.height = line_height;
        result.baseline = metrics.ascent;
        return finalize_bidi(std::move(result), text);
    }

    const float max_width = std::max(0.1f, constraints.max_width);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::size_t lines_before = result.lines.size();

        float current = 0.0f;
        bool has_line = false;
        std::size_t line_begin = 0;
        std::size_t line_end = 0;
        for (const WordRange& word : split_word_ranges(text, start, end)) {
            const std::string_view word_text = text.substr(word.begin, word.end - word.begin);
            const float word_width = measure_text(word_text);
            if (word_width > max_width) {
                if (has_line) {
                    add_line(current, line_begin, line_end);
                    current = 0.0f;
                    has_line = false;
                }
                // Split an over-wide word at the largest measured code-point prefix that fits.
                std::size_t chunk_begin = word.begin;
                while (chunk_begin < word.end) {
                    const std::string_view remaining =
                        text.substr(chunk_begin, word.end - chunk_begin);
                    const std::size_t codepoints = codepoint_count(remaining);
                    std::size_t best = 1;
                    std::size_t low = 1;
                    std::size_t high = codepoints;
                    while (low <= high) {
                        const std::size_t middle = low + (high - low) / 2;
                        const float width =
                            measure_text(remaining.substr(0, byte_offset(remaining, middle)));
                        if (width <= max_width) {
                            best = middle;
                            low = middle + 1;
                        } else if (middle == 1) {
                            break;
                        } else {
                            high = middle - 1;
                        }
                    }
                    const std::size_t chunk_end = chunk_begin + byte_offset(remaining, best);
                    add_line(measure_text(remaining.substr(0, chunk_end - chunk_begin)),
                             chunk_begin, chunk_end);
                    chunk_begin = chunk_end;
                }
                continue;
            }

            if (!has_line) {
                current = word_width;
                line_begin = word.begin;
                line_end = word.end;
                has_line = true;
            } else if (current + space_advance + word_width <= max_width) {
                current += space_advance + word_width;
                line_end = word.end;
            } else {
                add_line(current, line_begin, line_end);
                current = word_width;
                line_begin = word.begin;
                line_end = word.end;
                has_line = true;
            }
        }
        if (has_line) {
            add_line(current, line_begin, line_end);
        }
        if (result.lines.size() == lines_before) {
            // A blank (or whitespace-only) paragraph still owns a line box, so a caret after Enter
            // and a trailing newline land on their own line instead of the previous one.
            add_line(0.0f, start, end);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }

    if (result.lines.empty()) {
        add_line(0.0f, 0, text.size());
    }
    result.height = static_cast<float>(result.lines.size()) * line_height;
    result.baseline = metrics.ascent;
    return finalize_bidi(std::move(result), text);
}

} // namespace tessera::ui::text
