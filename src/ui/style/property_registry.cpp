// SPDX-License-Identifier: MIT

#include "tessera/ui/style/property_registry.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace tessera::ui::style {
namespace {

struct PropertyDef {
    const char* name;
    const char* initial;
    bool inherited;
};

constexpr PropertyDef kProperties[] = {
    {"display", "inline", false},
    {"position", "static", false},
    {"box-sizing", "content-box", false},
    {"width", "auto", false},
    {"height", "auto", false},
    {"min-width", "auto", false},
    {"min-height", "auto", false},
    {"max-width", "none", false},
    {"max-height", "none", false},
    {"top", "auto", false},
    {"right", "auto", false},
    {"bottom", "auto", false},
    {"left", "auto", false},
    {"margin-top", "0", false},
    {"margin-right", "0", false},
    {"margin-bottom", "0", false},
    {"margin-left", "0", false},
    {"padding-top", "0", false},
    {"padding-right", "0", false},
    {"padding-bottom", "0", false},
    {"padding-left", "0", false},
    {"border-top-width", "0", false},
    {"border-right-width", "0", false},
    {"border-bottom-width", "0", false},
    {"border-left-width", "0", false},
    {"border-top-style", "none", false},
    {"border-right-style", "none", false},
    {"border-bottom-style", "none", false},
    {"border-left-style", "none", false},
    {"border-top-color", "currentColor", false},
    {"border-right-color", "currentColor", false},
    {"border-bottom-color", "currentColor", false},
    {"border-left-color", "currentColor", false},
    {"border-top-left-radius", "0", false},
    {"border-top-right-radius", "0", false},
    {"border-bottom-right-radius", "0", false},
    {"border-bottom-left-radius", "0", false},
    {"color", "black", true},
    {"background-color", "transparent", false},
    {"opacity", "1", false},
    {"font-family", "sans-serif", true},
    {"font-size", "16px", true},
    {"font-style", "normal", true},
    {"font-weight", "400", true},
    {"line-height", "normal", true},
    {"letter-spacing", "normal", true},
    {"word-spacing", "normal", true},
    {"text-align", "start", true},
    {"text-transform", "none", true},
    {"white-space", "normal", true},
    {"visibility", "visible", true},
    {"cursor", "auto", true},
    {"direction", "ltr", true},
    {"writing-mode", "horizontal-tb", true},
    {"flex-direction", "row", false},
    {"flex-wrap", "nowrap", false},
    {"flex-grow", "0", false},
    {"flex-shrink", "1", false},
    {"flex-basis", "auto", false},
    {"align-items", "stretch", false},
    {"align-self", "auto", false},
    {"align-content", "normal", false},
    {"justify-content", "flex-start", false},
    {"justify-items", "legacy", false},
    {"row-gap", "normal", false},
    {"column-gap", "normal", false},
    {"overflow-x", "visible", false},
    {"overflow-y", "visible", false},
    {"order", "0", false},
    {"z-index", "auto", false},
    {"transform", "none", false},
    {"transform-origin", "50% 50%", false},
};

bool is_custom_property(std::string_view property) {
    return property.size() >= 2 && property[0] == '-' && property[1] == '-';
}

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

bool parse_number(std::string_view text, double& value) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

StyleDiagnostic invalid_shorthand(const Declaration& declaration, std::string_view property,
                                  std::string message) {
    return StyleDiagnostic{StyleDiagnosticKind::InvalidShorthand, declaration.offset,
                           std::string(property), std::move(message)};
}

Declaration make_declaration(const Declaration& source, std::string property, std::string value) {
    return Declaration{std::move(property), std::move(value), source.important, source.offset};
}

} // namespace

PropertyRegistry::PropertyRegistry() {
    for (const PropertyDef& definition : kProperties) {
        properties_[definition.name] = PropertyInfo{definition.initial, definition.inherited};
    }

    shorthands_["margin"] = ShorthandInfo{
        {"margin-top", "margin-right", "margin-bottom", "margin-left"}, ShorthandKind::Box4};
    shorthands_["padding"] = ShorthandInfo{
        {"padding-top", "padding-right", "padding-bottom", "padding-left"}, ShorthandKind::Box4};
    shorthands_["inset"] = ShorthandInfo{{"top", "right", "bottom", "left"}, ShorthandKind::Box4};
    shorthands_["border-width"] = ShorthandInfo{
        {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"},
        ShorthandKind::Box4};
    shorthands_["border-style"] = ShorthandInfo{
        {"border-top-style", "border-right-style", "border-bottom-style", "border-left-style"},
        ShorthandKind::Box4};
    shorthands_["border-color"] = ShorthandInfo{
        {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"},
        ShorthandKind::Box4};
    shorthands_["border-radius"] =
        ShorthandInfo{{"border-top-left-radius", "border-top-right-radius",
                       "border-bottom-right-radius", "border-bottom-left-radius"},
                      ShorthandKind::Box4};
    shorthands_["gap"] = ShorthandInfo{{"row-gap", "column-gap"}, ShorthandKind::Pair};
    shorthands_["overflow"] = ShorthandInfo{{"overflow-x", "overflow-y"}, ShorthandKind::Pair};
    shorthands_["place-items"] =
        ShorthandInfo{{"align-items", "justify-items"}, ShorthandKind::Pair};
    shorthands_["place-content"] =
        ShorthandInfo{{"align-content", "justify-content"}, ShorthandKind::Pair};
    shorthands_["flex"] =
        ShorthandInfo{{"flex-grow", "flex-shrink", "flex-basis"}, ShorthandKind::Flex};
}

const PropertyInfo* PropertyRegistry::find(std::string_view property) const {
    const std::string normalized = normalize_property(property);
    const auto found = properties_.find(normalized);
    return found == properties_.end() ? nullptr : &found->second;
}

bool PropertyRegistry::supported(std::string_view property) const {
    return is_custom_property(property) || find(property) != nullptr ||
           longhands(property) != nullptr;
}

const std::vector<std::string>* PropertyRegistry::longhands(std::string_view shorthand) const {
    const std::string normalized = normalize_property(shorthand);
    const auto found = shorthands_.find(normalized);
    return found == shorthands_.end() ? nullptr : &found->second.longhands;
}

PropertyExpansionResult PropertyRegistry::expand(const Declaration& declaration) const {
    PropertyExpansionResult result;
    const std::string property = normalize_property(declaration.property);

    if (is_custom_property(declaration.property)) {
        result.declarations.push_back(declaration);
        return result;
    }
    if (find(property) != nullptr) {
        Declaration normalized = declaration;
        normalized.property = property;
        result.declarations.push_back(std::move(normalized));
        return result;
    }

    const auto found = shorthands_.find(property);
    if (found == shorthands_.end()) {
        result.diagnostics.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedProperty,
                                                     declaration.offset, property,
                                                     "unsupported property"});
        return result;
    }

    switch (found->second.kind) {
    case ShorthandKind::Box4:
        return expand_box(declaration, found->second, property);
    case ShorthandKind::Pair:
        return expand_pair(declaration, found->second, property);
    case ShorthandKind::Flex:
        return expand_flex(declaration);
    }
    return result;
}

PropertyExpansionResult
PropertyRegistry::expand(const std::vector<Declaration>& declarations) const {
    PropertyExpansionResult result;
    for (const Declaration& declaration : declarations) {
        PropertyExpansionResult current = expand(declaration);
        result.declarations.insert(result.declarations.end(),
                                   std::make_move_iterator(current.declarations.begin()),
                                   std::make_move_iterator(current.declarations.end()));
        result.diagnostics.insert(result.diagnostics.end(),
                                  std::make_move_iterator(current.diagnostics.begin()),
                                  std::make_move_iterator(current.diagnostics.end()));
    }
    return result;
}

ComputedStyle PropertyRegistry::initial_style() const {
    ComputedStyle style;
    apply_initial_values(style);
    return style;
}

void PropertyRegistry::apply_initial_values(ComputedStyle& style) const {
    for (const auto& [name, info] : properties_) {
        if (!info.initial.empty() && !style.contains(name)) {
            style.set(name, info.initial);
        }
    }
}

ComputedStyle PropertyRegistry::compute_with_initial(const ComputedStyle* parent,
                                                     const CascadeTable& cascade) const {
    ComputedStyle style = compute_style(parent, cascade);
    apply_initial_values(style);
    return style;
}

PropertyExpansionResult PropertyRegistry::expand_box(const Declaration& declaration,
                                                     const ShorthandInfo& shorthand,
                                                     std::string_view property) const {
    PropertyExpansionResult result;
    const auto components = split_components(declaration.value);
    if (components.empty() || components.size() > 4) {
        result.diagnostics.push_back(
            invalid_shorthand(declaration, property, "expected one to four values"));
        return result;
    }

    const std::string_view top = components[0];
    const std::string_view right = components.size() > 1 ? components[1] : top;
    const std::string_view bottom = components.size() > 2 ? components[2] : top;
    const std::string_view left = components.size() > 3 ? components[3] : right;
    const std::string_view values[] = {top, right, bottom, left};

    for (std::size_t i = 0; i < shorthand.longhands.size(); ++i) {
        result.declarations.push_back(
            make_declaration(declaration, shorthand.longhands[i], std::string(values[i])));
    }
    return result;
}

PropertyExpansionResult PropertyRegistry::expand_pair(const Declaration& declaration,
                                                      const ShorthandInfo& shorthand,
                                                      std::string_view property) const {
    PropertyExpansionResult result;
    const auto components = split_components(declaration.value);
    if (components.empty() || components.size() > 2) {
        result.diagnostics.push_back(
            invalid_shorthand(declaration, property, "expected one or two values"));
        return result;
    }

    const std::string_view first = components[0];
    const std::string_view second = components.size() > 1 ? components[1] : first;
    result.declarations.push_back(
        make_declaration(declaration, shorthand.longhands[0], std::string(first)));
    result.declarations.push_back(
        make_declaration(declaration, shorthand.longhands[1], std::string(second)));
    return result;
}

PropertyExpansionResult PropertyRegistry::expand_flex(const Declaration& declaration) const {
    PropertyExpansionResult result;
    const auto components = split_components(declaration.value);
    if (components.empty() || components.size() > 3) {
        result.diagnostics.push_back(
            invalid_shorthand(declaration, "flex", "expected one to three values"));
        return result;
    }

    std::string grow = "1";
    std::string shrink = "1";
    std::string basis = "0%";

    if (components.size() == 1) {
        double number = 0.0;
        if (components[0] == "none") {
            grow = "0";
            shrink = "0";
            basis = "auto";
        } else if (components[0] == "auto") {
            grow = "1";
            shrink = "1";
            basis = "auto";
        } else if (parse_number(components[0], number)) {
            grow = std::string(components[0]);
            basis = "0%";
        } else {
            grow = "1";
            shrink = "1";
            basis = std::string(components[0]);
        }
    } else if (components.size() == 2) {
        grow = std::string(components[0]);
        double number = 0.0;
        if (parse_number(components[1], number)) {
            shrink = std::string(components[1]);
            basis = "0%";
        } else {
            shrink = "1";
            basis = std::string(components[1]);
        }
    } else {
        grow = std::string(components[0]);
        shrink = std::string(components[1]);
        basis = std::string(components[2]);
    }

    result.declarations.push_back(make_declaration(declaration, "flex-grow", std::move(grow)));
    result.declarations.push_back(make_declaration(declaration, "flex-shrink", std::move(shrink)));
    result.declarations.push_back(make_declaration(declaration, "flex-basis", std::move(basis)));
    return result;
}

} // namespace tessera::ui::style
