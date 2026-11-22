// SPDX-License-Identifier: MIT

#include "property_background.hpp"
#include "property_table.hpp"
#include "property_text_decoration.hpp"
#include "tessera/ui/style/backdrop_filter.hpp"
#include "tessera/ui/style/interaction_values.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/property_value_validation.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace tessera::ui::style {
namespace {

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

// Recognised properties may carry value forms V1 does not implement; each is reported once per
// declaration (stylesheet-v1 section 11: unsupported declarations are never silent). The
// box-shadow, transform and scroll grammars live in property_value_validation.cpp; the
// interaction properties validate here through their own shared grammar.
void report_unsupported_values(const Declaration& declaration, std::string_view property,
                               std::vector<StyleDiagnostic>& out) {
    std::string value(declaration.value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (is_interaction_value_property(property)) {
        report_unsupported_interaction_value(property, value, declaration.offset, out);
    }
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
    shorthands_["background"] =
        ShorthandInfo{{"background-color", "background-image"}, ShorthandKind::Pair};
    shorthands_["gap"] = ShorthandInfo{{"row-gap", "column-gap"}, ShorthandKind::Pair};
    shorthands_["overflow"] = ShorthandInfo{{"overflow-x", "overflow-y"}, ShorthandKind::Pair};
    shorthands_["place-items"] =
        ShorthandInfo{{"align-items", "justify-items"}, ShorthandKind::Pair};
    shorthands_["place-self"] = ShorthandInfo{{"align-self", "justify-self"}, ShorthandKind::Pair};
    shorthands_["place-content"] =
        ShorthandInfo{{"align-content", "justify-content"}, ShorthandKind::Pair};
    shorthands_["flex"] =
        ShorthandInfo{{"flex-grow", "flex-shrink", "flex-basis"}, ShorthandKind::Flex};
    register_logical_properties();
    register_extension_properties();
    build_defaults();
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

std::vector<std::string_view> PropertyRegistry::longhand_names() const {
    std::vector<std::string_view> names;
    names.reserve(properties_.size());
    for (const auto& [name, info] : properties_) {
        (void)info;
        names.push_back(name);
    }
    return names;
}

PropertyExpansionResult PropertyRegistry::expand(const Declaration& declaration) const {
    PropertyExpansionResult result;
    std::string property = normalize_property(declaration.property);

    if (is_custom_property(declaration.property)) {
        result.declarations.push_back(declaration);
        return result;
    }
    // `-webkit-backdrop-filter` is an alias of `backdrop-filter` (glass-v1 section 2): one computed
    // value, so an author cannot have the two spellings fight in the cascade.
    if (property == "-webkit-backdrop-filter") {
        property = "backdrop-filter";
    }
    if (property == "backdrop-filter") {
        // The whole declaration is ignored when any function is unsupported, never just the
        // unsupported part (glass-v1 section 2). The computed value therefore stays at the initial
        // `none` instead of holding a list the paint pass would have to re-validate.
        const BackdropFilterParseResult parsed = parse_backdrop_filter(declaration.value);
        if (!parsed.ok()) {
            std::string message = "unsupported backdrop-filter value";
            if (!parsed.token.empty()) {
                message += " `";
                message += parsed.token;
                message += "`";
            }
            message += ": ";
            message += parsed.error;
            message += "; the whole declaration is ignored";
            result.diagnostics.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue,
                                                         declaration.offset, property,
                                                         std::move(message)});
            return result;
        }
    }
    // CSS allows `text-overflow: <overflow> <side>`, which V1 does not implement. Reject the whole
    // declaration instead of applying half of it: no winner reaches the cascade, so the computed
    // value stays at the initial `clip` and the diagnostic names the unsupported form.
    if (property == "text-overflow" && split_components(declaration.value).size() != 1) {
        result.diagnostics.push_back(
            invalid_shorthand(declaration, property,
                              "text-overflow takes one value (clip or ellipsis); the two-value "
                              "<overflow> <side> shorthand is not supported in V1"));
        return result;
    }
    // `window-backdrop` is a closed keyword set. An invalid value drops the declaration (the
    // computed value keeps its initial `none`) and reports it once, rather than letting an
    // unparseable string reach the window's startup request.
    if (property == "window-backdrop") {
        const auto components = split_components(declaration.value);
        if (components.size() != 1 || !is_window_backdrop_value(components.front())) {
            result.diagnostics.push_back(
                StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue, declaration.offset, property,
                                "window-backdrop takes one of none, auto, mica, acrylic or blur"});
            return result;
        }
    }
    if (property == "background" || property == "text-decoration") {
        return property == "background" ? expand_background_shorthand(declaration)
                                        : expand_text_decoration_shorthand(declaration);
    }
    if (find(property) != nullptr) {
        Declaration normalized = make_declaration(declaration, property, declaration.value);
        report_unsupported_values(normalized, property, result.diagnostics);
        if (!validate_property_value(normalized, property, result.diagnostics)) {
            return result;
        }
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
    case ShorthandKind::BorderSide:
        return expand_border_side(declaration, found->second, property);
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
    // No entries of its own: the defaults are its whole effective style, so this is one allocation.
    ComputedStyle style;
    style.attach_defaults(defaults_);
    return style;
}

void PropertyRegistry::apply_initial_values(ComputedStyle& style) const {
    // The style is a delta over `defaults_`, so its initial values are already the fallback: there
    // is nothing to materialize (issue #735, slice F). Attaching is what makes them visible.
    style.attach_defaults(defaults_);
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
