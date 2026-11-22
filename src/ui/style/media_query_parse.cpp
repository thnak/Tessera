// SPDX-License-Identifier: MIT
//
// The `@media` parser: conditions, the range syntax and the query list (issue #691). Split out of
// `media_query.cpp`, which keeps the evaluator and is at its source-size budget.
//
// The parser and the evaluator share the feature-name mapping and the comparison operators, so
// those live in `media_query_internal.hpp` rather than being duplicated.

#include "media_query_internal.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/media_query.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

using tessera::detail::lower;
using tessera::detail::split_top_level;

std::string trim(std::string_view value) {
    return std::string(tessera::detail::trim(value));
}

bool starts_with_word(std::string_view text, std::string_view word) {
    if (text.size() < word.size() || text.substr(0, word.size()) != word) {
        return false;
    }
    return text.size() == word.size() ||
           std::isspace(static_cast<unsigned char>(text[word.size()])) != 0;
}

} // namespace

bool feature_from_name(std::string_view name, MediaFeature& feature) {
    if (name == "width") {
        feature = MediaFeature::Width;
    } else if (name == "height") {
        feature = MediaFeature::Height;
    } else if (name == "resolution") {
        feature = MediaFeature::Resolution;
    } else if (name == "aspect-ratio") {
        feature = MediaFeature::AspectRatio;
    } else if (name == "orientation") {
        feature = MediaFeature::Orientation;
    } else if (name == "prefers-color-scheme") {
        feature = MediaFeature::PrefersColorScheme;
    } else if (name == "prefers-reduced-motion") {
        feature = MediaFeature::PrefersReducedMotion;
    } else if (name == "window-backdrop") {
        feature = MediaFeature::WindowBackdrop;
    } else if (name == "hover") {
        feature = MediaFeature::Hover;
    } else if (name == "any-hover") {
        feature = MediaFeature::AnyHover;
    } else if (name == "pointer") {
        feature = MediaFeature::Pointer;
    } else if (name == "any-pointer") {
        feature = MediaFeature::AnyPointer;
    } else {
        return false;
    }
    return true;
}

bool is_range_feature(MediaFeature feature) {
    return feature == MediaFeature::Width || feature == MediaFeature::Height ||
           feature == MediaFeature::Resolution || feature == MediaFeature::AspectRatio;
}

bool comparison_from_operator(std::string_view text, MediaComparison& comparison) {
    if (text == ">=") {
        comparison = MediaComparison::GreaterEqual;
    } else if (text == "<=") {
        comparison = MediaComparison::LessEqual;
    } else if (text == ">") {
        comparison = MediaComparison::Greater;
    } else if (text == "<") {
        comparison = MediaComparison::Less;
    } else if (text == "=") {
        comparison = MediaComparison::Equal;
    } else {
        return false;
    }
    return true;
}

MediaComparison flip_comparison(MediaComparison comparison) {
    switch (comparison) {
    case MediaComparison::Greater:
        return MediaComparison::Less;
    case MediaComparison::GreaterEqual:
        return MediaComparison::LessEqual;
    case MediaComparison::Less:
        return MediaComparison::Greater;
    case MediaComparison::LessEqual:
        return MediaComparison::GreaterEqual;
    case MediaComparison::Equal:
    case MediaComparison::Min:
    case MediaComparison::Max:
        break;
    }
    return comparison;
}

namespace {

// The range form (issue #691): `(width >= 600px)`, `(600px <= width)`, and the two-sided
// `(600px <= width <= 900px)`. The feature may sit on either side of the operator, and a two-sided
// range must have the feature in the middle with the operators pointing the same way.
bool parse_range_condition(std::string_view content, MediaCondition& condition) {
    // Split on the comparison operators, keeping them: `600px <= width <= 900px` becomes
    // ["600px", "<=", "width", "<=", "900px"].
    std::vector<std::string> parts;
    std::size_t index = 0;
    while (index < content.size()) {
        const std::size_t op_at = content.find_first_of("<>=", index);
        if (op_at == std::string_view::npos) {
            parts.push_back(trim(content.substr(index)));
            break;
        }
        parts.push_back(trim(content.substr(index, op_at - index)));
        std::size_t op_end = op_at + 1;
        if (op_end < content.size() && content[op_end] == '=' && content[op_at] != '=') {
            ++op_end;
        }
        parts.push_back(std::string(content.substr(op_at, op_end - op_at)));
        index = op_end;
    }
    // Drop the empty leading/trailing pieces a leading or trailing operator produces.
    std::erase_if(parts, [](const std::string& part) { return part.empty(); });
    if (parts.size() != 3 && parts.size() != 5) {
        return false;
    }

    const auto is_operator = [](const std::string& text) {
        MediaComparison ignored = MediaComparison::Equal;
        return comparison_from_operator(text, ignored);
    };

    if (parts.size() == 3) {
        if (is_operator(parts[1])) {
            // `feature op value` or `value op feature`.
            MediaFeature feature = MediaFeature::Width;
            if (feature_from_name(lower(parts[0]), feature) && is_range_feature(feature)) {
                condition.feature = feature;
                if (!comparison_from_operator(parts[1], condition.comparison)) {
                    return false;
                }
                condition.value = parts[2];
                return true;
            }
            if (feature_from_name(lower(parts[2]), feature) && is_range_feature(feature)) {
                condition.feature = feature;
                // `600px <= width` means `width >= 600px`, so the operator flips.
                MediaComparison flipped = MediaComparison::Equal;
                if (!comparison_from_operator(parts[1], flipped)) {
                    return false;
                }
                condition.comparison = flip_comparison(flipped);
                condition.value = parts[0];
                return true;
            }
        }
        return false;
    }

    // Two-sided: `value op feature op value`, both operators pointing the same way.
    MediaFeature feature = MediaFeature::Width;
    if (!is_operator(parts[1]) || !is_operator(parts[3]) ||
        !feature_from_name(lower(parts[2]), feature) || !is_range_feature(feature)) {
        return false;
    }
    MediaComparison lower_op = MediaComparison::Equal;
    MediaComparison upper_op = MediaComparison::Equal;
    if (!comparison_from_operator(parts[1], lower_op) ||
        !comparison_from_operator(parts[3], upper_op)) {
        return false;
    }
    // `600px <= width <= 900px` is the ascending form; the descending form is not valid CSS.
    const bool ascending =
        (lower_op == MediaComparison::Less || lower_op == MediaComparison::LessEqual) &&
        (upper_op == MediaComparison::Less || upper_op == MediaComparison::LessEqual);
    const bool descending =
        (lower_op == MediaComparison::Greater || lower_op == MediaComparison::GreaterEqual) &&
        (upper_op == MediaComparison::Greater || upper_op == MediaComparison::GreaterEqual);
    if (!ascending && !descending) {
        return false;
    }
    condition.feature = feature;
    if (ascending) {
        condition.comparison = MediaComparison::GreaterEqual;
        condition.value = parts[0];
        condition.upper_comparison = MediaComparison::LessEqual;
        condition.upper_value = parts[4];
    } else {
        condition.comparison = MediaComparison::LessEqual;
        condition.value = parts[0];
        condition.upper_comparison = MediaComparison::GreaterEqual;
        condition.upper_value = parts[4];
    }
    return true;
}

bool parse_condition(std::string_view inner, MediaCondition& condition) {
    const std::string text = lower(trim(inner));
    if (text.empty() || text.front() != '(' || text.back() != ')') {
        return false;
    }
    const std::string content = trim(std::string_view(text).substr(1, text.size() - 2));

    // The range form first: it has no colon, and a feature name can appear on either side.
    if (content.find(':') == std::string::npos) {
        return parse_range_condition(content, condition);
    }

    const std::size_t colon = content.find(':');
    std::string name = trim(std::string_view(content).substr(0, colon));
    const std::string value = trim(std::string_view(content).substr(colon + 1));

    condition.value = value;
    // The legacy `min-`/`max-` prefixes are inclusive bounds on a range feature.
    MediaComparison prefix = MediaComparison::Equal;
    if (name.rfind("min-", 0) == 0) {
        prefix = MediaComparison::Min;
        name.erase(0, 4);
    } else if (name.rfind("max-", 0) == 0) {
        prefix = MediaComparison::Max;
        name.erase(0, 4);
    }
    MediaFeature feature = MediaFeature::Width;
    if (!feature_from_name(name, feature)) {
        return false;
    }
    // A prefix on a feature that is not a range (`min-orientation`) is not valid CSS.
    if (prefix != MediaComparison::Equal && !is_range_feature(feature)) {
        return false;
    }
    condition.feature = feature;
    condition.comparison = prefix;
    return true;
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

} // namespace tessera::ui::style
