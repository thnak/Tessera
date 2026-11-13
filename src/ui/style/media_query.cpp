// SPDX-License-Identifier: MIT

#include "tessera/ui/style/media_query.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
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

bool starts_with_word(std::string_view text, std::string_view word) {
    if (text.size() < word.size() || text.substr(0, word.size()) != word) {
        return false;
    }
    return text.size() == word.size() ||
           std::isspace(static_cast<unsigned char>(text[word.size()])) != 0;
}

std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            --depth;
        } else if (c == separator && depth == 0) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

bool parse_number_value(std::string_view text, double& value, std::string_view& unit) {
    std::size_t index = 0;
    while (index < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[index])) != 0 || text[index] == '.' ||
            text[index] == '+' || text[index] == '-')) {
        ++index;
    }
    if (index == 0) {
        return false;
    }
    const std::string number(text.substr(0, index));
    char* end = nullptr;
    value = std::strtod(number.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    unit = text.substr(index);
    return true;
}

bool parse_length(std::string_view text, const MediaContext& context, double& pixels) {
    double number = 0.0;
    std::string_view unit;
    if (!parse_number_value(text, number, unit)) {
        return false;
    }
    if (unit == "px" || unit.empty()) {
        pixels = number;
        return true;
    }
    if (unit == "rem" || unit == "em") {
        pixels = number * 16.0;
        return true;
    }
    if (unit == "vw") {
        pixels = context.viewport_width * number / 100.0;
        return true;
    }
    if (unit == "vh") {
        pixels = context.viewport_height * number / 100.0;
        return true;
    }
    return false;
}

bool parse_resolution(std::string_view text, double& dpi) {
    double number = 0.0;
    std::string_view unit;
    if (!parse_number_value(text, number, unit)) {
        return false;
    }
    if (unit == "dpi") {
        dpi = number;
        return true;
    }
    if (unit == "dppx") {
        dpi = number * 96.0;
        return true;
    }
    if (unit == "dpcm") {
        dpi = number * 2.54;
        return true;
    }
    return false;
}

bool parse_condition(std::string_view inner, MediaCondition& condition) {
    const std::string text = lower(trim(inner));
    if (text.empty() || text.front() != '(' || text.back() != ')') {
        return false;
    }
    const std::string content = trim(std::string_view(text).substr(1, text.size() - 2));
    const std::size_t colon = content.find(':');
    const std::string name = trim(std::string_view(content).substr(0, colon));
    const std::string value = colon == std::string::npos
                                  ? std::string()
                                  : trim(std::string_view(content).substr(colon + 1));

    condition.value = value;
    if (name == "min-width") {
        condition.feature = MediaFeature::Width;
        condition.comparison = MediaComparison::Min;
    } else if (name == "max-width") {
        condition.feature = MediaFeature::Width;
        condition.comparison = MediaComparison::Max;
    } else if (name == "width") {
        condition.feature = MediaFeature::Width;
        condition.comparison = MediaComparison::Equal;
    } else if (name == "min-height") {
        condition.feature = MediaFeature::Height;
        condition.comparison = MediaComparison::Min;
    } else if (name == "max-height") {
        condition.feature = MediaFeature::Height;
        condition.comparison = MediaComparison::Max;
    } else if (name == "height") {
        condition.feature = MediaFeature::Height;
        condition.comparison = MediaComparison::Equal;
    } else if (name == "min-resolution") {
        condition.feature = MediaFeature::Resolution;
        condition.comparison = MediaComparison::Min;
    } else if (name == "max-resolution") {
        condition.feature = MediaFeature::Resolution;
        condition.comparison = MediaComparison::Max;
    } else if (name == "resolution") {
        condition.feature = MediaFeature::Resolution;
        condition.comparison = MediaComparison::Equal;
    } else if (name == "orientation") {
        condition.feature = MediaFeature::Orientation;
    } else if (name == "prefers-color-scheme") {
        condition.feature = MediaFeature::PrefersColorScheme;
    } else if (name == "prefers-reduced-motion") {
        condition.feature = MediaFeature::PrefersReducedMotion;
    } else if (name == "window-backdrop") {
        condition.feature = MediaFeature::WindowBackdrop;
    } else {
        return false;
    }
    return colon != std::string::npos;
}

bool parse_query(std::string_view text, MediaQuery& query) {
    std::string remaining = lower(trim(text));
    if (starts_with_word(remaining, "not")) {
        query.negated = true;
        remaining = trim(std::string_view(remaining).substr(3));
    } else if (starts_with_word(remaining, "only")) {
        remaining = trim(std::string_view(remaining).substr(4));
    }

    if (!remaining.empty() && remaining.front() != '(') {
        const std::size_t end = remaining.find_first_of(" \t\r\n");
        query.type = remaining.substr(0, end);
        remaining = end == std::string::npos ? std::string()
                                             : trim(std::string_view(remaining).substr(end));
        if (!remaining.empty()) {
            if (!starts_with_word(remaining, "and")) {
                return false;
            }
            remaining = trim(std::string_view(remaining).substr(3));
        }
        if (query.type != "all" && query.type != "screen" && query.type != "print" &&
            query.type != "speech") {
            return false;
        }
    }

    if (remaining.empty()) {
        return !query.type.empty();
    }

    for (const std::string_view part : split_top_level(remaining, ' ')) {
        const std::string term = trim(part);
        if (term.empty()) {
            continue;
        }
        if (term == "and") {
            continue;
        }
        MediaCondition condition;
        if (!parse_condition(term, condition)) {
            return false;
        }
        query.conditions.push_back(std::move(condition));
    }
    return !query.conditions.empty();
}

bool compare_number(double actual, double expected, MediaComparison comparison) {
    switch (comparison) {
    case MediaComparison::Min:
        return actual >= expected;
    case MediaComparison::Max:
        return actual <= expected;
    case MediaComparison::Equal:
        return std::abs(actual - expected) < 0.5;
    }
    return false;
}

// The `window-backdrop` grammar of glass-v1.md section 2: `none | mica | acrylic | blur` on the
// resolved material. Kept local (a keyword comparison, not a HAL call) so the style library has no
// link dependency on the window HAL; `auto` is a request and therefore never matches.
bool backdrop_keyword_matches(std::string_view value, hal::BackdropMaterial material) {
    switch (material) {
    case hal::BackdropMaterial::NoMaterial:
        return value == "none";
    case hal::BackdropMaterial::Mica:
        return value == "mica";
    case hal::BackdropMaterial::Acrylic:
        return value == "acrylic";
    case hal::BackdropMaterial::Blur:
        return value == "blur";
    }
    return false;
}

bool evaluate_condition(const MediaCondition& condition, const MediaContext& context) {
    const std::string value = lower(trim(condition.value));
    switch (condition.feature) {
    case MediaFeature::Width: {
        double expected = 0.0;
        return parse_length(value, context, expected) &&
               compare_number(context.viewport_width, expected, condition.comparison);
    }
    case MediaFeature::Height: {
        double expected = 0.0;
        return parse_length(value, context, expected) &&
               compare_number(context.viewport_height, expected, condition.comparison);
    }
    case MediaFeature::Resolution: {
        double expected = 0.0;
        return parse_resolution(value, expected) &&
               compare_number(context.resolution_dpi, expected, condition.comparison);
    }
    case MediaFeature::Orientation:
        if (value == "landscape") {
            return context.viewport_width >= context.viewport_height;
        }
        if (value == "portrait") {
            return context.viewport_width < context.viewport_height;
        }
        return false;
    case MediaFeature::PrefersColorScheme:
        if (value == "light") {
            return context.color_scheme == ColorScheme::Light;
        }
        if (value == "dark") {
            return context.color_scheme == ColorScheme::Dark;
        }
        return false;
    case MediaFeature::PrefersReducedMotion:
        if (value == "reduce") {
            return context.prefers_reduced_motion;
        }
        if (value == "no-preference") {
            return !context.prefers_reduced_motion;
        }
        return false;
    case MediaFeature::WindowBackdrop:
        // Unknown keywords (including `auto`) do not match, exactly like an unknown `orientation`
        // value. `none` matches only the resolved "no material" answer, so a stylesheet can carry
        // its own opaque fallback.
        return backdrop_keyword_matches(value, context.window_backdrop);
    }
    return false;
}

} // namespace

MediaQueryParseResult parse_media_query_list(std::string_view text) {
    MediaQueryParseResult result;
    for (const std::string_view part : split_top_level(text, ',')) {
        const std::string query_text = trim(part);
        if (query_text.empty()) {
            continue;
        }
        MediaQuery query;
        if (!parse_query(query_text, query)) {
            result.ok = false;
            result.error_offset = 0;
            continue;
        }
        result.list.queries.push_back(std::move(query));
    }
    if (result.list.queries.empty()) {
        result.ok = false;
    }
    return result;
}

bool evaluate(const MediaQuery& query, const MediaContext& context) {
    bool matched = query.type.empty() || query.type == "all" || query.type == "screen";
    for (const MediaCondition& condition : query.conditions) {
        matched = matched && evaluate_condition(condition, context);
    }
    return query.negated ? !matched : matched;
}

bool evaluate(const MediaQueryList& queries, const MediaContext& context) {
    for (const MediaQuery& query : queries.queries) {
        if (evaluate(query, context)) {
            return true;
        }
    }
    return false;
}

} // namespace tessera::ui::style
