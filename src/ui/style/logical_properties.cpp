// SPDX-License-Identifier: MIT
//
// Inline-axis (direction-relative) properties and their shorthands (stylesheet-v1 section 11, issue
// #230). The registry table in property_registry.cpp holds the physical longhands; this translation
// unit adds the logical ones with **empty initial values**, so a logical declaration is an explicit
// inline-axis override and an undeclared one leaves the physical value in effect. Layout maps the
// longhands to Yoga's Start/End edges; paint resolves the same mapping from the computed
// `direction`, so one stylesheet mirrors between LTR and RTL.

#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/property_registry.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

// Every logical longhand is a plain box/border value, so none inherits. See the file comment for
// why `initial` is empty.
constexpr const char* kLogicalLonghands[] = {
    "margin-inline-start",       "margin-inline-end",         "padding-inline-start",
    "padding-inline-end",        "inset-inline-start",        "inset-inline-end",
    "border-inline-start-width", "border-inline-start-style", "border-inline-start-color",
    "border-inline-end-width",   "border-inline-end-style",   "border-inline-end-color",
};

constexpr std::string_view kWidthDefault = "0";
constexpr std::string_view kStyleDefault = "none";
constexpr std::string_view kColorDefault = "currentColor";

std::string lower(std::string_view value) {
    std::string result(value);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

// Split a shorthand value on top-level whitespace. Quoted strings and balanced (), [] are kept
// together so `rgb(1, 2, 3)` and a family list stay one component. Mirrors the registry but stays
// local so this translation unit can expand a side without widening the registry's interface.
std::vector<std::string_view> split_components(std::string_view value) {
    std::vector<std::string_view> components;
    std::size_t start = std::string_view::npos;
    int depth = 0;
    char quote = 0;

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < value.size()) {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            --depth;
        } else if (std::isspace(static_cast<unsigned char>(c)) != 0 && depth == 0) {
            if (start != std::string_view::npos) {
                components.push_back(value.substr(start, i - start));
                start = std::string_view::npos;
            }
            continue;
        }
        if (start == std::string_view::npos && std::isspace(static_cast<unsigned char>(c)) == 0) {
            start = i;
        }
    }
    if (start != std::string_view::npos) {
        components.push_back(value.substr(start));
    }
    return components;
}

Declaration make_declaration(const Declaration& source, const std::string& property,
                             std::string value) {
    return Declaration{property, std::move(value), source.important, source.offset};
}

StyleDiagnostic invalid_side(const Declaration& declaration, std::string_view property,
                             std::string message) {
    return StyleDiagnostic{StyleDiagnosticKind::InvalidShorthand, declaration.offset,
                           std::string(property), std::move(message)};
}

bool is_border_style(std::string_view token) {
    return token == "none" || token == "hidden" || token == "solid" || token == "dotted" ||
           token == "dashed" || token == "double" || token == "groove" || token == "ridge" ||
           token == "inset" || token == "outset";
}

// A length is a number (with its unit) or one of the three border-width keywords. Anything else is
// classified as a color below, and an unparseable token is reported instead of being guessed.
bool is_border_width(std::string_view token) {
    if (token == "thin" || token == "medium" || token == "thick") {
        return true;
    }
    if (token.empty()) {
        return false;
    }
    const char first = token.front();
    return std::isdigit(static_cast<unsigned char>(first)) != 0 || first == '.' || first == '+' ||
           first == '-';
}

bool is_color(std::string_view token) {
    return lower(token) == "currentcolor" || parse_color(token).ok;
}

} // namespace

void PropertyRegistry::register_logical_properties() {
    for (const char* name : kLogicalLonghands) {
        properties_[name] = PropertyInfo{std::string(), false};
    }

    shorthands_["margin-inline"] =
        ShorthandInfo{{"margin-inline-start", "margin-inline-end"}, ShorthandKind::Pair};
    shorthands_["padding-inline"] =
        ShorthandInfo{{"padding-inline-start", "padding-inline-end"}, ShorthandKind::Pair};
    shorthands_["inset-inline"] =
        ShorthandInfo{{"inset-inline-start", "inset-inline-end"}, ShorthandKind::Pair};
    shorthands_["border-inline-width"] = ShorthandInfo{
        {"border-inline-start-width", "border-inline-end-width"}, ShorthandKind::Pair};
    shorthands_["border-inline-style"] = ShorthandInfo{
        {"border-inline-start-style", "border-inline-end-style"}, ShorthandKind::Pair};
    shorthands_["border-inline-color"] = ShorthandInfo{
        {"border-inline-start-color", "border-inline-end-color"}, ShorthandKind::Pair};
    shorthands_["border-inline-start"] = ShorthandInfo{
        {"border-inline-start-width", "border-inline-start-style", "border-inline-start-color"},
        ShorthandKind::BorderSide};
    shorthands_["border-inline-end"] = ShorthandInfo{
        {"border-inline-end-width", "border-inline-end-style", "border-inline-end-color"},
        ShorthandKind::BorderSide};
}

// `<width> || <style> || <color>` in any order, as in CSS. Omitted components reset to their
// initial values (`0`, `none`, `currentColor`), which is what a shorthand does: the logical side
// then fully specifies the physical side it maps to, instead of half-overriding it.
PropertyExpansionResult PropertyRegistry::expand_border_side(const Declaration& declaration,
                                                             const ShorthandInfo& shorthand,
                                                             std::string_view property) const {
    PropertyExpansionResult result;
    const std::vector<std::string_view> components = split_components(declaration.value);
    if (components.empty() || components.size() > 3) {
        result.diagnostics.push_back(
            invalid_side(declaration, property, "expected a width, a style and a color"));
        return result;
    }

    std::array<std::string, 3> values = {std::string(kWidthDefault), std::string(kStyleDefault),
                                         std::string(kColorDefault)};
    std::array<bool, 3> seen = {false, false, false};
    for (const std::string_view component : components) {
        const std::string token = lower(component);
        std::size_t slot = 0;
        if (is_border_style(token)) {
            slot = 1;
        } else if (is_border_width(token)) {
            slot = 0;
        } else if (is_color(component)) {
            slot = 2;
        } else {
            result.diagnostics.push_back(invalid_side(declaration, property,
                                                      "unsupported `" + std::string(component) +
                                                          "` in a logical border side"));
            return result;
        }
        if (seen[slot]) {
            result.diagnostics.push_back(
                invalid_side(declaration, property,
                             "a logical border side takes at most one width, style and color"));
            return result;
        }
        seen[slot] = true;
        values[slot] = std::string(component);
    }

    for (const std::string& longhand : shorthand.longhands) {
        std::size_t slot = 0;
        if (longhand.ends_with("-style")) {
            slot = 1;
        } else if (longhand.ends_with("-color")) {
            slot = 2;
        }
        result.declarations.push_back(make_declaration(declaration, longhand, values[slot]));
    }
    return result;
}

} // namespace tessera::ui::style
