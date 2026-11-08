// SPDX-License-Identifier: MIT

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/property_registry.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::style::CascadeTable;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::PropertyExpansionResult;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleDiagnosticKind;
using tessera::ui::style::StyleNode;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "property_registry: " << message << "\n";
    }
    return condition;
}

const Declaration* find_declaration(const PropertyExpansionResult& result,
                                    std::string_view property) {
    for (const Declaration& declaration : result.declarations) {
        if (declaration.property == property) {
            return &declaration;
        }
    }
    return nullptr;
}

bool decl_is(const PropertyExpansionResult& result, std::string_view property,
             std::string_view value) {
    const Declaration* declaration = find_declaration(result, property);
    return declaration != nullptr && declaration->value == value;
}

bool value_is(const ComputedStyle& style, const char* property, std::string_view expected) {
    const std::string* value = style.find(property);
    return value != nullptr && *value == expected;
}

} // namespace

int main() {
    PropertyRegistry registry;

    const auto* color = registry.find("color");
    const auto* display = registry.find("display");
    if (!expect(color != nullptr && color->inherited && color->initial == "black", "color info") ||
        !expect(display != nullptr && !display->inherited && display->initial == "inline",
                "display info") ||
        !expect(registry.find("unknown") == nullptr, "unknown property") ||
        !expect(registry.supported("--token"), "custom property supported") ||
        !expect(registry.supported("margin"), "shorthand supported") ||
        !expect(!registry.supported("background"), "unsupported shorthand") ||
        !expect(registry.longhands("margin") != nullptr &&
                    registry.longhands("margin")->size() == 4,
                "margin longhands") ||
        !expect(registry.longhands("color") == nullptr, "longhand has no expansion")) {
        return 1;
    }

    PropertyExpansionResult result =
        registry.expand(Declaration{"margin", "1px 2px 3px 4px", true, 7});
    if (!expect(result.diagnostics.empty(), "margin diagnostics") ||
        !expect(result.declarations.size() == 4, "margin count") ||
        !expect(decl_is(result, "margin-top", "1px"), "margin top") ||
        !expect(decl_is(result, "margin-right", "2px"), "margin right") ||
        !expect(decl_is(result, "margin-bottom", "3px"), "margin bottom") ||
        !expect(decl_is(result, "margin-left", "4px"), "margin left") ||
        !expect(result.declarations[0].important && result.declarations[0].offset == 7,
                "margin metadata")) {
        return 2;
    }

    result = registry.expand(Declaration{"padding", "1px 2px", false, 0});
    if (!expect(decl_is(result, "padding-top", "1px"), "padding top") ||
        !expect(decl_is(result, "padding-right", "2px"), "padding right") ||
        !expect(decl_is(result, "padding-bottom", "1px"), "padding bottom") ||
        !expect(decl_is(result, "padding-left", "2px"), "padding left")) {
        return 3;
    }

    result = registry.expand(Declaration{"inset", "0", false, 0});
    if (!expect(decl_is(result, "top", "0") && decl_is(result, "right", "0") &&
                    decl_is(result, "bottom", "0") && decl_is(result, "left", "0"),
                "inset expansion")) {
        return 4;
    }

    result = registry.expand(Declaration{"border-radius", "1px 2px 3px 4px", false, 0});
    if (!expect(decl_is(result, "border-top-left-radius", "1px"), "radius top-left") ||
        !expect(decl_is(result, "border-top-right-radius", "2px"), "radius top-right") ||
        !expect(decl_is(result, "border-bottom-right-radius", "3px"), "radius bottom-right") ||
        !expect(decl_is(result, "border-bottom-left-radius", "4px"), "radius bottom-left")) {
        return 5;
    }

    result = registry.expand(Declaration{"gap", "4px 6px", false, 0});
    if (!expect(decl_is(result, "row-gap", "4px") && decl_is(result, "column-gap", "6px"),
                "gap expansion")) {
        return 6;
    }

    result = registry.expand(Declaration{"overflow", "hidden auto", false, 0});
    if (!expect(decl_is(result, "overflow-x", "hidden") && decl_is(result, "overflow-y", "auto"),
                "overflow expansion")) {
        return 7;
    }

    result = registry.expand(Declaration{"place-items", "center stretch", false, 0});
    if (!expect(decl_is(result, "align-items", "center") &&
                    decl_is(result, "justify-items", "stretch"),
                "place-items expansion")) {
        return 8;
    }

    result = registry.expand(Declaration{"flex", "1", false, 0});
    if (!expect(decl_is(result, "flex-grow", "1") && decl_is(result, "flex-shrink", "1") &&
                    decl_is(result, "flex-basis", "0%"),
                "flex one value")) {
        return 9;
    }

    result = registry.expand(Declaration{"flex", "1 0 10px", false, 0});
    if (!expect(decl_is(result, "flex-grow", "1") && decl_is(result, "flex-shrink", "0") &&
                    decl_is(result, "flex-basis", "10px"),
                "flex three values")) {
        return 10;
    }

    result = registry.expand(Declaration{"flex", "auto", false, 0});
    if (!expect(decl_is(result, "flex-grow", "1") && decl_is(result, "flex-shrink", "1") &&
                    decl_is(result, "flex-basis", "auto"),
                "flex auto")) {
        return 11;
    }

    result = registry.expand(Declaration{"margin", "1px 2px 3px 4px 5px", false, 0});
    if (!expect(result.declarations.empty(), "invalid margin declarations") ||
        !expect(result.diagnostics.size() == 1, "invalid margin diagnostic") ||
        !expect(result.diagnostics[0].kind == StyleDiagnosticKind::InvalidShorthand,
                "invalid margin kind")) {
        return 12;
    }

    result = registry.expand(Declaration{"background", "red", false, 0});
    if (!expect(result.declarations.empty(), "unsupported declarations") ||
        !expect(result.diagnostics.size() == 1, "unsupported diagnostic") ||
        !expect(result.diagnostics[0].kind == StyleDiagnosticKind::UnsupportedProperty,
                "unsupported kind")) {
        return 13;
    }

    result = registry.expand(Declaration{"--Token", "value", false, 0});
    if (!expect(result.declarations.size() == 1, "custom declaration") ||
        !expect(result.declarations[0].property == "--Token", "custom case preserved")) {
        return 14;
    }

    result = registry.expand(Declaration{"Color", "red", false, 0});
    if (!expect(result.declarations.size() == 1, "longhand declaration") ||
        !expect(result.declarations[0].property == "color", "longhand normalized")) {
        return 15;
    }

    const PropertyExpansionResult many = registry.expand(
        {Declaration{"padding", "1px", false, 0}, Declaration{"background", "red", false, 0}});
    if (!expect(many.declarations.size() == 4, "vector expansion declarations") ||
        !expect(many.diagnostics.size() == 1, "vector expansion diagnostics")) {
        return 16;
    }

    const ComputedStyle initial = registry.initial_style();
    if (!expect(value_is(initial, "display", "inline"), "initial display") ||
        !expect(value_is(initial, "color", "black"), "initial color") ||
        !expect(value_is(initial, "margin-top", "0"), "initial margin") ||
        !expect(value_is(initial, "font-size", "16px"), "initial font-size") ||
        !expect(value_is(initial, "opacity", "1"), "initial opacity")) {
        return 17;
    }

    ComputedStyle parent;
    parent.set("color", "red");
    parent.set("font-size", "20px");
    parent.set("width", "100px");
    CascadeTable child_cascade;
    child_cascade.add({"color", "blue", false, tessera::ui::style::Origin::Author, {}, 1});
    const ComputedStyle child = registry.compute_with_initial(&parent, child_cascade);
    if (!expect(value_is(child, "color", "blue"), "computed child color") ||
        !expect(value_is(child, "font-size", "20px"), "computed inherited font") ||
        !expect(value_is(child, "display", "inline"), "computed initial display") ||
        !expect(value_is(child, "width", "auto"), "computed initial width") ||
        !expect(value_is(child, "margin-top", "0"), "computed initial margin")) {
        return 18;
    }

    StyleNode card;
    card.tag = "div";
    card.classes = {"card"};
    const auto compiled = compile_stylesheet(".card { margin: 4px; unknown-prop: 1; }");
    if (!expect(compiled.rules().size() == 1, "compiled rule") ||
        !expect(compiled.index().size() == 1, "compiled index") ||
        !expect(compiled.diagnostics().size() == 1, "compiled diagnostic") ||
        !expect(compiled.diagnostics()[0].kind == StyleDiagnosticKind::UnsupportedProperty,
                "compiled diagnostic kind")) {
        return 19;
    }

    const ComputedStyle compiled_style = compiled.compute(card, nullptr);
    if (!expect(value_is(compiled_style, "margin-top", "4px"), "compiled margin") ||
        !expect(value_is(compiled_style, "display", "inline"), "compiled initial display") ||
        !expect(!compiled_style.contains("unknown-prop"), "unsupported not computed")) {
        return 20;
    }

    std::cout << "property_registry: ok\n";
    return 0;
}
