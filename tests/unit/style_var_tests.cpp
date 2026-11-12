// SPDX-License-Identifier: MIT

// var() resolution end to end (stylesheet-v1 section 9): the compiled stylesheet resolves theme
// tokens through the cascade and inheritance, a shorthand expands before substitution, and an
// inline declaration resolves against cascade and inline custom properties, falling back to
// `unset` semantics when a reference cannot resolve.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace style = tessera::ui::style;
namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "style_var: " << message << "\n";
        ++failures;
    }
}

void check_value(const style::ComputedStyle& computed, std::string_view property,
                 std::string_view expected, std::string_view message) {
    const std::string* value = computed.find(property);
    if (value == nullptr || *value != expected) {
        std::cerr << "style_var: " << message << " (" << property << " = "
                  << (value == nullptr ? "<absent>" : *value) << ", expected " << expected << ")\n";
        ++failures;
    }
}

void attach(style::StyleNode& parent, style::StyleNode& child) {
    child.parent = &parent;
    parent.children.push_back(&child);
}

// The cascade resolves a theme token for a descendant and a shorthand expands to longhands before
// substitution, so `padding: var(--gap)` reaches all four sides.
void test_cascade_and_inheritance() {
    style::StyleNode root;
    root.tag = "root";
    root.id = "app";
    style::StyleNode card;
    card.tag = "view";
    card.id = "card";
    card.classes = {"card"};
    attach(root, card);

    const style::CompiledStyleSheet sheet =
        style::compile_stylesheet("#app { --accent: #102030; --gap: 8px; }\n"
                                  ".card { color: var(--accent); padding: var(--gap); "
                                  "border-top-width: var(--missing, 2px); }\n");

    const style::ComputedStyle root_style = sheet.compute(root, nullptr);
    check_value(root_style, "--accent", "#102030", "custom property computed on the root");

    const style::ComputedStyle card_style = sheet.compute(card, &root_style);
    check_value(card_style, "color", "#102030", "var() resolves through inheritance");
    check_value(card_style, "padding-top", "8px", "var() in an expanded shorthand (top)");
    check_value(card_style, "padding-right", "8px", "var() in an expanded shorthand (right)");
    check_value(card_style, "padding-bottom", "8px", "var() in an expanded shorthand (bottom)");
    check_value(card_style, "padding-left", "8px", "var() in an expanded shorthand (left)");
    check_value(card_style, "border-top-width", "2px", "fallback for a missing token");
}

// Inline declarations win over the cascade and may define or override custom properties themselves;
// the second resolution pass sees both. An unresolvable inline reference behaves like `unset`, so
// an inherited property keeps the parent's computed value.
void test_inline_declarations() {
    ui::Node root(1);
    root.set_element_kind("View");
    root.set_element_id("app");
    ui::Node& card = root.add_child(std::make_unique<ui::Node>(2));
    card.set_element_kind("View");
    card.set_element_id("card");
    card.set_class_name("card");

    const style::CompiledStyleSheet sheet =
        style::compile_stylesheet("#app { color: red; --accent: purple; }\n"
                                  ".card { color: teal; }\n");

    check(card.set_inline_style("background-color", "var(--accent)"),
          "inline var over a cascade custom property is accepted");
    check(card.set_inline_style("--gap", "6px"), "inline custom property is accepted");
    check(card.set_inline_style("padding-left", "var(--gap)"),
          "inline var over an inline custom property is accepted");
    check(card.set_inline_style("color", "var(--missing)"), "inline unresolved var is accepted");

    style::SceneStyleTree tree(root);
    const style::ComputedStyle root_style = tree.compute(sheet, root);
    const style::ComputedStyle card_style = tree.compute(sheet, card);

    check_value(root_style, "color", "red", "root color comes from the cascade");
    check_value(card_style, "background-color", "purple",
                "inline var sees a cascade custom property");
    check_value(card_style, "padding-left", "6px", "inline var sees an inline custom property");
    check_value(card_style, "color", "red",
                "an unresolved inline var falls back to the parent value");
}

// A cycle cannot survive inline application either: both members become invalid and a consumer
// uses its fallback.
void test_inline_cycle() {
    ui::Node root(3);
    root.set_element_kind("View");
    check(root.set_inline_style("--a", "var(--b)"), "cycle member a is accepted");
    check(root.set_inline_style("--b", "var(--a)"), "cycle member b is accepted");
    check(root.set_inline_style("background-color", "var(--a, blue)"),
          "cycle consumer is accepted");

    const style::CompiledStyleSheet sheet = style::compile_stylesheet("");
    style::SceneStyleTree tree(root);
    const style::ComputedStyle computed = tree.compute(sheet, root);
    check(!computed.contains("--a") && !computed.contains("--b"),
          "inline cycle members are invalid");
    check_value(computed, "background-color", "blue", "inline cycle consumer uses the fallback");
}

} // namespace

int main() {
    test_cascade_and_inheritance();
    test_inline_declarations();
    test_inline_cycle();
    if (failures != 0) {
        std::cerr << "style_var: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "style_var: ok\n";
    return 0;
}
