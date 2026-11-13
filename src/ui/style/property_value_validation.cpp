// SPDX-License-Identifier: MIT

#include "tessera/ui/style/property_value_validation.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

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

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// True when `word` appears in `text` as a whole keyword (not inside `inset-x` or a longer name).
bool has_keyword(std::string_view text, std::string_view word) {
    std::size_t at = text.find(word);
    while (at != std::string_view::npos) {
        const bool left_ok = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) ||
                                          text[at - 1] == '-');
        const std::size_t end = at + word.size();
        const bool right_ok =
            end >= text.size() ||
            !(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '-');
        if (left_ok && right_ok) {
            return true;
        }
        at = text.find(word, at + 1);
    }
    return false;
}

// The scroll runtime properties are single-keyword grammars in V1 (docs/specs/stylesheet-v1.md
// section 11); the two-value forms CSS allows are not implemented and are reported as unsupported.
bool is_single_keyword(std::string_view value, std::initializer_list<std::string_view> allowed) {
    const std::string_view token = trim(value);
    if (token.empty() || token.find_first_of(" \t\r\n\f") != std::string_view::npos) {
        return false;
    }
    return std::find(allowed.begin(), allowed.end(), token) != allowed.end();
}

bool is_scroll_style_property(std::string_view property) {
    return property == "scroll-behavior" || property == "overscroll-behavior" ||
           property == "overscroll-effect" || property == "scrollbar-gutter";
}

bool scroll_style_value_supported(std::string_view property, std::string_view value) {
    if (property == "scroll-behavior") {
        return is_single_keyword(value, {"auto", "smooth"});
    }
    if (property == "overscroll-behavior") {
        return is_single_keyword(value, {"auto", "contain", "none"});
    }
    // The exact names of docs/specs/animation-v1.md section 7 (OverscrollEffect).
    if (property == "overscroll-effect") {
        return is_single_keyword(value, {"none", "glow", "stretch", "bounce", "platform"});
    }
    if (property == "scrollbar-gutter") {
        return is_single_keyword(value, {"auto", "stable"});
    }
    return true;
}

const char* scroll_style_message(std::string_view property) {
    if (property == "scroll-behavior") {
        return "scroll-behavior takes one of auto or smooth";
    }
    if (property == "overscroll-behavior") {
        return "overscroll-behavior takes one of auto, contain or none";
    }
    if (property == "overscroll-effect") {
        return "overscroll-effect takes one of none, glow, stretch, bounce or platform";
    }
    return "scrollbar-gutter takes one of auto or stable";
}

} // namespace

bool validate_property_value(const Declaration& declaration, std::string_view property,
                             std::vector<StyleDiagnostic>& out) {
    const std::string value = lower(declaration.value);
    if (property == "box-shadow" && has_keyword(value, "inset")) {
        out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                      std::string(property),
                                      "inset shadows are not supported in V1 and are not painted"});
    } else if (property == "transform" &&
               (value.find("3d(") != std::string::npos || has_keyword(value, "perspective") ||
                value.find("perspective(") != std::string::npos)) {
        out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                      std::string(property),
                                      "3D transform functions are not supported in V1"});
    }
    if (!is_scroll_style_property(property)) {
        return true;
    }
    // A custom-property reference is substituted after this check; keep the declaration so the
    // computed-value pass can judge the resolved text.
    if (trim(value).find("var(") != std::string_view::npos) {
        return true;
    }
    if (scroll_style_value_supported(property, value)) {
        return true;
    }
    out.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset,
                                  std::string(property), scroll_style_message(property)});
    return false;
}

} // namespace tessera::ui::style
