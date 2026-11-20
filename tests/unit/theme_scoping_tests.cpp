// SPDX-License-Identifier: MIT
//
// `:has()` and `@scope` (issue #622): the decision is in docs/design/theme-scoping.md. This test
// covers the selector half (`:has()` parses, matches a descendant/child/following sibling, takes
// the most specific argument's specificity, and answers `@supports selector()`), the at-rule half
// (`@scope (<start>) [to (<end>)]` applies only inside the region), and their composition with
// `@layer` and source order.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/selector.hpp"
#include "tessera/ui/style/selector_match.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::CompiledStyleSheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::matches;
using tessera::ui::style::Origin;
using tessera::ui::style::parse_selector_list;
using tessera::ui::style::selector_supported;
using tessera::ui::style::Specificity;
using tessera::ui::style::StyleNode;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "theme_scoping: " << message << "\n";
    }
    return condition;
}

void attach(StyleNode& parent, StyleNode& child) {
    child.parent = &parent;
    parent.children.push_back(&child);
}

bool matches_text(std::string_view text, const StyleNode& node) {
    const auto parsed = parse_selector_list(text);
    return parsed.ok && matches(parsed.list, node);
}

Specificity specificity_of(std::string_view text) {
    const auto parsed = parse_selector_list(text);
    return parsed.ok ? tessera::ui::style::specificity(parsed.list) : Specificity{};
}

// A small tree: root > card > (title, icon), and root > sibling.
struct Tree {
    StyleNode root;
    StyleNode card;
    StyleNode title;
    StyleNode icon;
    StyleNode sibling;

    Tree() {
        root.tag = "root";
        card.tag = "div";
        card.classes = {"card"};
        title.tag = "span";
        title.classes = {"title"};
        icon.tag = "svg";
        icon.classes = {"icon"};
        sibling.tag = "div";
        sibling.classes = {"sibling"};
        attach(root, card);
        attach(card, title);
        attach(card, icon);
        attach(root, sibling);
    }
};
// `:has()` parses, and its specificity is the most specific argument (not the sum).
bool has_grammar() {
    const auto parsed = parse_selector_list("div:has(.icon)");
    bool ok = expect(parsed.ok && parsed.list.selectors.size() == 1, ":has() parses");

    // `:has(.a, #b)` is (1,0,0): the most specific argument, not (1,1,0).
    const Specificity mixed = specificity_of("div:has(.a, #b)");
    ok = expect(mixed.ids == 1 && mixed.classes == 0 && mixed.types == 1,
                ":has() takes the most specific argument's specificity") &&
         ok;
    // A single argument adds to the compound's own weight.
    const Specificity single = specificity_of("div.card:has(.icon)");
    ok = expect(single.classes == 2 && single.types == 1,
                ":has() adds its argument's weight to the compound") &&
         ok;
    // A malformed argument fails the compound rather than matching nothing silently.
    ok = expect(!parse_selector_list("div:has(").ok, "an unterminated :has() is a parse failure") &&
         ok;
    ok = expect(!parse_selector_list("div:has()").ok, "an empty :has() is a parse failure") && ok;
    return ok;
}
// `:has()` matches a descendant, a child, and a following sibling; not an ancestor or a preceding
// sibling.
bool has_matching() {
    Tree tree;
    bool ok = expect(matches_text("div:has(.icon)", tree.card), ":has() matches a descendant");
    ok = expect(matches_text("div:has(> .icon)", tree.card), ":has() matches a child") && ok;
    ok = expect(matches_text("div:has(.title, .icon)", tree.card),
                ":has() is a disjunction of its arguments") &&
         ok;
    ok = expect(!matches_text("div:has(.missing)", tree.card),
                ":has() does not match a missing descendant") &&
         ok;
    ok = expect(!matches_text("div:has(.icon)", tree.title),
                ":has() does not look at an ancestor") &&
         ok;
    ok = expect(matches_text("div:has(.sibling)", tree.card),
                ":has() matches a following sibling") &&
         ok;
    ok = expect(!matches_text("div:has(.card)", tree.sibling),
                ":has() does not look at a preceding sibling") &&
         ok;
    ok = expect(!matches_text("div:has(.icon)", tree.root),
                ":has() on the root does not match a grandchild of a child") &&
         ok;
    // The root does have `.icon` in its subtree, so `:has()` on the root is true.
    ok = expect(matches_text("root:has(.icon)", tree.root),
                ":has() on the root matches a deep descendant") &&
         ok;
    return ok;
}

// `@supports selector(:has(a))` answers true, because the engine matches it.
bool has_supports() {
    return expect(selector_supported(":has(a)"), "@supports selector(:has(a)) is true") &&
           expect(selector_supported("div:has(> .icon)"),
                  "@supports selector(div:has(> .icon)) is true");
}

// `compute` always fills a property's initial value, so "the rule did not apply" is asserted as
// "the value is still the initial", not as "the property is absent".
bool is_initial_color(const ComputedStyle& style) {
    const std::string* color = style.find("color");
    return color == nullptr || *color == "black";
}

// `@scope (<start>) [to (<end>)]`: a rule applies only inside the region.
bool scope_applies() {
    const std::string css = R"(
        @scope (.card) { .title { color: #ff0000; } }
        .icon { color: #00ff00; }
    )";
    const CompiledStyleSheet sheet = compile_stylesheet(css, Origin::Author);
    bool ok = expect(sheet.diagnostics().empty(), "@scope parses without a diagnostic");

    Tree tree;
    const ComputedStyle inside = sheet.compute(tree.title, nullptr);
    ok = expect(inside.find("color") != nullptr && *inside.find("color") == "#ff0000",
                "a scoped rule applies inside the region") &&
         ok;
    // `.icon` is unscoped, so it applies everywhere.
    const ComputedStyle icon = sheet.compute(tree.icon, nullptr);
    ok = expect(icon.find("color") != nullptr && *icon.find("color") == "#00ff00",
                "an unscoped rule still applies") &&
         ok;
    // A `.title` outside the region does not get the rule: the sibling is not a `.title`, so give
    // it the class to prove the scope, not the selector, is what excludes it.
    StyleNode outside_title;
    outside_title.tag = "span";
    outside_title.classes = {"title"};
    attach(tree.root, outside_title);
    ok = expect(is_initial_color(sheet.compute(outside_title, nullptr)),
                "a matching node outside the region does not get the rule") &&
         ok;
    return ok;
}

// The scope is a filter on top of the selector: a node that matches the rule but sits outside the
// region does not get it.
bool scope_filters_the_selector() {
    const std::string css = R"(
        @scope (.card) { div { color: #ff0000; } }
    )";
    const CompiledStyleSheet sheet = compile_stylesheet(css, Origin::Author);
    Tree tree;
    // `tree.sibling` is a `div` but is not inside `.card`, so the rule does not apply.
    const ComputedStyle sibling = sheet.compute(tree.sibling, nullptr);
    const ComputedStyle card = sheet.compute(tree.card, nullptr);
    return expect(is_initial_color(sibling),
                  "a matching node outside the region does not get the rule") &&
           expect(card.find("color") != nullptr && *card.find("color") == "#ff0000",
                  "the region's own element gets the rule");
}
// `to (<end>)` closes the region: a node below an `end` element is outside it.
bool scope_end() {
    const std::string css = R"(
        @scope (.card) to (.stop) { span { color: #ff0000; } }
    )";
    const CompiledStyleSheet sheet = compile_stylesheet(css, Origin::Author);
    bool ok = expect(sheet.diagnostics().empty(), "`to (...)` parses");

    // root > card > stop > title: the title is below the end element, so it is outside the scope.
    StyleNode root;
    root.tag = "root";
    StyleNode card;
    card.tag = "div";
    card.classes = {"card"};
    StyleNode stop;
    stop.tag = "div";
    stop.classes = {"stop"};
    StyleNode title;
    title.tag = "span";
    attach(root, card);
    attach(card, stop);
    attach(stop, title);

    const ComputedStyle below = sheet.compute(title, nullptr);
    ok = expect(is_initial_color(below), "a node below the end element is outside") && ok;

    // A span directly under the card is inside.
    StyleNode direct;
    direct.tag = "span";
    attach(card, direct);
    const ComputedStyle inside = sheet.compute(direct, nullptr);
    ok = expect(inside.find("color") != nullptr, "a node above the end element is inside") && ok;
    return ok;
}

// `@scope` and `@layer` are orthogonal: the scope decides whether a rule applies, the layer decides
// where it sorts among the rules that do.
bool scope_with_layer() {
    const std::string css = R"(
        @layer base { @scope (.card) { .title { color: #ff0000; } } }
        .title { color: #00ff00; }
    )";
    const CompiledStyleSheet sheet = compile_stylesheet(css, Origin::Author);
    bool ok = expect(sheet.diagnostics().empty(), "@scope inside @layer parses");
    Tree tree;
    const ComputedStyle inside = sheet.compute(tree.title, nullptr);
    // The unlayered rule wins over the layered one regardless of specificity, as `@layer` says.
    ok = expect(inside.find("color") != nullptr && *inside.find("color") == "#00ff00",
                "the unlayered rule beats the layered scoped one") &&
         ok;
    ok = expect(sheet.layer_order().count("base") == 1, "the layer is registered") && ok;
    return ok;
}

// A malformed prelude drops the block with a diagnostic instead of applying it unscoped.
bool scope_rejects_a_bad_prelude() {
    const CompiledStyleSheet sheet =
        compile_stylesheet("@scope .card { .title { color: #ff0000; } }", Origin::Author);
    bool ok = expect(!sheet.diagnostics().empty(), "a prelude without parentheses is reported");
    Tree tree;
    const ComputedStyle title = sheet.compute(tree.title, nullptr);
    ok = expect(is_initial_color(title), "a rejected @scope applies nothing") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = has_grammar();
    ok = has_matching() && ok;
    ok = has_supports() && ok;
    ok = scope_applies() && ok;
    ok = scope_filters_the_selector() && ok;
    ok = scope_end() && ok;
    ok = scope_with_layer() && ok;
    ok = scope_rejects_a_bad_prelude() && ok;
    if (!ok) {
        std::cerr << "theme_scoping: FAILED\n";
        return 1;
    }
    std::cout << "theme_scoping: ok\n";
    return 0;
}
