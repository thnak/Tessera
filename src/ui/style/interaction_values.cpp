// SPDX-License-Identifier: MIT

#include "tessera/ui/style/interaction_values.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace tessera::ui::style {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

bool equals_lower(std::string_view text, std::string_view keyword) {
    if (text.size() != keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != keyword[i]) {
            return false;
        }
    }
    return true;
}

// A CSS `<number>` that can take part in a ratio: positive and finite. Both ratio terms must be
// positive, so `0` and negative values are rejected instead of reaching Yoga as a degenerate ratio.
bool is_positive_number(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    double value = 0.0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last && value > 0.0;
}

bool is_aspect_ratio_value(std::string_view value) {
    if (equals_lower(value, "auto")) {
        return true;
    }
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos) {
        return is_positive_number(value);
    }
    if (value.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    return is_positive_number(value.substr(0, slash)) &&
           is_positive_number(value.substr(slash + 1));
}

bool is_css_wide_keyword(std::string_view value) {
    return equals_lower(value, "inherit") || equals_lower(value, "initial") ||
           equals_lower(value, "unset") || equals_lower(value, "revert") ||
           equals_lower(value, "revert-layer") || value.find("var(") != std::string_view::npos;
}

} // namespace

bool is_interaction_value_property(std::string_view property) {
    return property == "pointer-events" || property == "user-select" || property == "aspect-ratio";
}

bool is_supported_interaction_value(std::string_view property, std::string_view value) {
    const std::string_view normalized = trim(value);
    if (property == "pointer-events") {
        return equals_lower(normalized, "auto") || equals_lower(normalized, "none");
    }
    if (property == "user-select") {
        return equals_lower(normalized, "auto") || equals_lower(normalized, "none") ||
               equals_lower(normalized, "text");
    }
    if (property == "aspect-ratio") {
        return is_aspect_ratio_value(normalized);
    }
    return true;
}

void report_unsupported_interaction_value(std::string_view property, std::string_view value,
                                          std::size_t offset, std::vector<StyleDiagnostic>& out) {
    if (!is_interaction_value_property(property)) {
        return;
    }
    const std::string_view normalized = trim(value);
    if (is_css_wide_keyword(normalized) || is_supported_interaction_value(property, normalized)) {
        return;
    }
    std::string message(property);
    message += " does not support the value `";
    message += normalized;
    message += "`";
    if (property == "pointer-events") {
        message += " (V1 implements auto and none; the SVG-only keywords are not supported)";
    } else if (property == "user-select") {
        message += " (V1 implements auto, none and text)";
    } else {
        message += " (V1 implements auto, a positive <number> or <number> / <number>)";
    }
    out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, offset,
                                  std::string(property), std::move(message)});
}

} // namespace tessera::ui::style
