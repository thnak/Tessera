// SPDX-License-Identifier: MIT

#include "tessera/ui/style/text_decoration.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace tessera::ui::style {
namespace {

constexpr const char* kSpace = " \t\r\n\f";

std::string lower_trimmed(std::string_view value) {
    const auto first = value.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(kSpace);
    std::string result(value.substr(first, last - first + 1));
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool parse_double(std::string_view text, double& out) {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last && std::isfinite(out);
}

// A signed px/em/rem length (or a bare zero), in px.
std::optional<float> parse_signed_length(std::string_view value, float font_size) {
    const std::string token = lower_trimmed(value);
    if (token.empty()) {
        return std::nullopt;
    }
    std::string_view number = token;
    float unit = 1.0f;
    if (token.size() > 3 && token.ends_with("rem")) {
        number = std::string_view(token).substr(0, token.size() - 3);
        unit = font_size;
    } else if (token.size() > 2 && token.ends_with("em")) {
        number = std::string_view(token).substr(0, token.size() - 2);
        unit = font_size;
    } else if (token.size() > 2 && token.ends_with("px")) {
        number = std::string_view(token).substr(0, token.size() - 2);
    } else if (token != "0" && token != "+0" && token != "-0") {
        return std::nullopt;
    }
    if (!number.empty() && number.front() == '+') {
        number.remove_prefix(1);
    }
    double parsed = 0.0;
    if (!parse_double(number, parsed)) {
        return std::nullopt;
    }
    return static_cast<float>(parsed * unit);
}

} // namespace

std::optional<std::uint8_t> parse_text_decoration_line(std::string_view value) {
    const std::string text = lower_trimmed(value);
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint8_t mask = 0;
    bool saw_none = false;
    int count = 0;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t end = std::min(text.find_first_of(kSpace, position), text.size());
        const std::string_view word = std::string_view(text).substr(position, end - position);
        position = std::min(text.find_first_not_of(kSpace, end), text.size());
        std::uint8_t bit = 0;
        if (word == "none") {
            saw_none = true;
        } else if (word == "underline") {
            bit = kDecorationUnderline;
        } else if (word == "overline") {
            bit = kDecorationOverline;
        } else if (word == "line-through") {
            bit = kDecorationLineThrough;
        } else {
            return std::nullopt;
        }
        if ((mask & bit) != 0) {
            return std::nullopt;
        }
        mask = static_cast<std::uint8_t>(mask | bit);
        ++count;
    }
    if (saw_none && count != 1) {
        return std::nullopt;
    }
    return mask;
}

bool is_supported_text_decoration_style(std::string_view value) {
    return lower_trimmed(value) == "solid";
}

std::optional<float> parse_decoration_length(std::string_view value, float font_size) {
    const std::optional<float> parsed = parse_signed_length(value, font_size);
    if (!parsed || *parsed < 0.0f) {
        return std::nullopt;
    }
    return parsed;
}

bool is_supported_text_decoration_thickness(std::string_view value) {
    const std::string token = lower_trimmed(value);
    return token == "auto" || token == "from-font" ||
           parse_decoration_length(token, 16.0f).has_value();
}

float text_decoration_thickness(std::string_view value, float font_size) {
    return parse_decoration_length(value, font_size).value_or(0.0f);
}

bool is_supported_vertical_align(std::string_view value) {
    const std::string token = lower_trimmed(value);
    // `top`, `middle` and `bottom` place the content of a table cell in its row (layout_style.cpp,
    // docs/design/table-layout.md section 3); on text they shift nothing.
    return token == "baseline" || token == "sub" || token == "super" || token == "top" ||
           token == "middle" || token == "bottom" || parse_signed_length(token, 16.0f).has_value();
}

float vertical_align_shift(std::string_view value, float font_size) {
    const std::string token = lower_trimmed(value);
    if (token == "sub") {
        return -kSubShiftEm * font_size;
    }
    if (token == "super") {
        return kSuperShiftEm * font_size;
    }
    return parse_signed_length(token, font_size).value_or(0.0f);
}

} // namespace tessera::ui::style
