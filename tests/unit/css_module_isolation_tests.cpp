// SPDX-License-Identifier: MIT

// Issue #463, the acceptance test of docs/design/css-isolation.md: two components with a `.title`
// class in their own scoped sheets do not affect each other, and a global rule still reaches what
// it always reached. It runs on the engine's own cascade (the compiled sheet and `SceneStyleTree`),
// the code the native app and the Wasm preview share, so it is the proof for both.
//
// A test that only passes with scoping must be shown to fail without it, so the last case compiles
// the same three sheets UNSCOPED and asserts they collide. If scoping were removed, the scoped case
// would then see the same collision and fail.

#include "tessera/compiler/css_module/module_scan.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace css = tessera::compiler::css_module;
namespace style = tessera::ui::style;
using tessera::ui::Node;

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "css_module_isolation: " << message << "\n";
        ++g_failures;
    }
}

// Two component sheets that both declare `.title` with a different padding, and the app's global
// sheet, which also names `.title`. Spelled as text so the expectation is readable.
constexpr std::string_view kCardCss = ".title { padding-top: 7px; }\n";
constexpr std::string_view kPanelCss = ".title { padding-top: 13px; }\n";
constexpr std::string_view kGlobalCss = ".title { padding-top: 99px; }\n";

struct Tree {
    std::unique_ptr<Node> root;
    Node* first = nullptr;
    Node* second = nullptr;
    Node* global = nullptr;
};

// A root with three children carrying the given class names, in order.
Tree make_tree(std::string_view first, std::string_view second, std::string_view global) {
    Tree tree;
    tree.root = std::make_unique<Node>(1);
    const auto add = [&](tessera::ui::NodeId id, std::string_view name) {
        Node& node = tree.root->add_child(std::make_unique<Node>(id));
        node.set_class_name(name);
        return &node;
    };
    tree.first = add(2, first);
    tree.second = add(3, second);
    tree.global = add(4, global);
    return tree;
}

std::string padding_top(const style::CompiledStyleSheet& sheet, const Tree& tree,
                        const Node& node) {
    style::SceneStyleTree styles(*tree.root);
    const style::ComputedStyle computed = styles.compute(sheet, node);
    const std::string* value = computed.find("padding-top");
    return value != nullptr ? *value : std::string();
}

// The three sheets in app order: the two components' sheets, then the global sheet.
style::CompiledStyleSheet compile_in_order(std::string_view card, std::string_view panel,
                                           std::string_view global) {
    const style::PreparedStyleSheet card_sheet = style::prepare_stylesheet(card);
    const style::PreparedStyleSheet panel_sheet = style::prepare_stylesheet(panel);
    const style::PreparedStyleSheet global_sheet = style::prepare_stylesheet(global);
    const std::vector<style::StylesheetSource> sources{
        {"ui/Card.module.css", &card_sheet, style::Origin::Author, false},
        {"ui/Panel.module.css", &panel_sheet, style::Origin::Author, false},
        {"ui/app.css", &global_sheet, style::Origin::Author, false},
    };
    return style::CompiledStyleSheet(sources);
}

// What the compiler produces for a module file: the scoped text and the scoped `title` class.
struct Scoped {
    std::string text;
    std::string title;
};

Scoped scope(std::string_view path, std::string_view text) {
    const auto scanned = css::scan_module_css(path, text);
    expect(scanned.has_value(), "the module file `" + std::string(path) + "` scans");
    if (!scanned) {
        return {};
    }
    std::string title;
    for (const css::ModuleClass& item : scanned->classes) {
        if (item.local == "title") {
            title = item.scoped;
        }
    }
    return {scanned->rewritten, title};
}

void test_scoped_sheets_do_not_affect_each_other() {
    const Scoped card = scope("ui/Card.module.css", kCardCss);
    const Scoped panel = scope("ui/Panel.module.css", kPanelCss);
    expect(!card.title.empty() && !panel.title.empty(), "both files declare a scoped `title`");
    expect(card.title != panel.title, "the same authored name scopes differently per file: `" +
                                          card.title + "` vs `" + panel.title + "`");

    // The tree is what the lowering produces: each component carries its own scoped class, and a
    // plain `className="title"` (a global class) sits beside them.
    const Tree tree = make_tree(card.title, panel.title, "title");
    const style::CompiledStyleSheet sheet = compile_in_order(card.text, panel.text, kGlobalCss);

    expect(padding_top(sheet, tree, *tree.first) == "7px",
           "Card's `.title` styles only Card, got `" + padding_top(sheet, tree, *tree.first) + "`");
    expect(padding_top(sheet, tree, *tree.second) == "13px",
           "Panel's `.title` styles only Panel, got `" + padding_top(sheet, tree, *tree.second) +
               "`");
    // The global rule still reaches what it always reached, and reaches nothing it did not.
    expect(padding_top(sheet, tree, *tree.global) == "99px",
           "a global `.title` still reaches a plain `title`, got `" +
               padding_top(sheet, tree, *tree.global) + "`");
}

// The anti-vacuity half: with the three sheets compiled as written, the same class name is one rule
// for everyone and the last sheet wins on all three nodes. This is the defect scoping removes.
void test_the_same_sheets_collide_without_scoping() {
    const Tree tree = make_tree("title", "title", "title");
    const style::CompiledStyleSheet sheet = compile_in_order(kCardCss, kPanelCss, kGlobalCss);
    const std::string first = padding_top(sheet, tree, *tree.first);
    const std::string second = padding_top(sheet, tree, *tree.second);
    expect(first == "99px" && second == "99px",
           "unscoped, every `.title` takes the last sheet's rule (got `" + first + "` and `" +
               second + "`), so Card and Panel cannot be told apart");
}

} // namespace

int main() {
    test_scoped_sheets_do_not_affect_each_other();
    test_the_same_sheets_collide_without_scoping();
    if (g_failures != 0) {
        std::cerr << "css module isolation tests: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "css module isolation tests: all passed\n";
    return 0;
}
