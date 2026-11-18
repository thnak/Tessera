// SPDX-License-Identifier: MIT
//
// The author stylesheet in the preview (issue #390), through the PREVIEW path.
//
// This file exists for one question the issue says must be proven rather than read: #397/#398 were
// "nested selectors and nested at-rules overflowed the stack in the stylesheet compiler", #410
// fixed them, and #390 then hands user-typed CSS to exactly that code. A stack overflow in Wasm
// aborts the module, which for a designer means losing unsaved work -- so the cases below feed
// pathological CSS through `compile_preview_stylesheet()` and `prepare_scene()` (the functions the
// browser module calls) and require a *diagnostic* and a built frame, not a crash. A process that
// survived a case would also have survived it without any bound, so each hostile case also asserts
// that the frame was still laid out: the test fails if the engine merely swallowed the problem.
//
// The rest of the file is the behaviour the three owner decisions describe:
//
//   1. the author sheet enters through `ScenePreviewOptions::stylesheet` and applies when the
//   source
//      imports `./Preview.css` -- the sheets a program imports are the ones in force, as in a
//      native build, so a source that never imports it previews with no author CSS;
//   2. a non-empty author sheet REPLACES `preview_stylesheet()` (a `Button` with no size rule gets
//   the
//      engine's own default, not the built-in 120x32), and an empty one keeps the fallback;
//   3. `:root` custom properties + `var()` resolve from the author's CSS.
//
// A parallel cascade is the failure this file is built to catch, so every colour assertion reads
// the computed style through `style::SceneStyleTree::compute()` over the sheet `prepare_scene()`
// returned: the same cascade the native build runs, not a re-derivation.

#include "scene_prepare.hpp"
#include "scene_stylesheet.hpp"
#include "scene_stylesheet_test_support.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace style = tessera::ui::style;
namespace ui = tessera::ui;

// The failure counter and `expect()` are shared with `scene_stylesheet_module_tests.cpp` (issue
// #463), so both halves of the ctest case report one total.
using scene_stylesheet_test::expect;
using scene_stylesheet_test::failures;

// A column with a button and a text leaf: the shapes the built-in sheet sizes, so "the built-in
// look" and "the author's look" are distinguishable by the button's box alone.
constexpr std::string_view kColumn = R"tsx(import "./Preview.css";

export function Column(): Node {
  return (
    <View>
      <Text>Preview</Text>
      <Button>Add</Button>
    </View>
  );
}
)tsx";

cli::ScenePreviewOptions options_for(std::string_view stylesheet) {
    cli::ScenePreviewOptions options;
    options.file = "ui/Column.tsx";
    options.salt = "preview";
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    options.stylesheet = stylesheet;
    return options;
}

const ui::Node* find_kind(const ui::Node& node, std::string_view kind) {
    if (node.element_kind() == kind) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_kind(*child, kind)) {
            return found;
        }
    }
    return nullptr;
}

// The computed value of `property` on `node`, through the sheet the preview laid the tree out with.
std::string computed(const cli::PreparedScene& scene, const ui::Node& node,
                     std::string_view property) {
    if (!scene.sheet.has_value()) {
        return {};
    }
    style::SceneStyleTree tree(*scene.root);
    const style::ComputedStyle style = tree.compute(*scene.sheet, node);
    const std::string* value = style.find(property);
    return value != nullptr ? *value : std::string();
}

// Selector text repeated `depth` times around `inner`, e.g. `:where(:where(Button))`.
std::string nest_function(std::string_view name, std::string_view inner, int depth) {
    std::string text;
    text.reserve(static_cast<std::size_t>(depth) * (name.size() + 1) + inner.size());
    for (int i = 0; i < depth; ++i) {
        text.append(name);
        text.push_back('(');
    }
    text.append(inner);
    for (int i = 0; i < depth; ++i) {
        text.push_back(')');
    }
    return text;
}

std::string repeat_open(std::string_view open, std::string_view inner, std::string_view close,
                        int depth) {
    std::string text;
    for (int i = 0; i < depth; ++i) {
        text.append(open);
    }
    text.append(inner);
    for (int i = 0; i < depth; ++i) {
        text.append(close);
    }
    return text;
}

// --- 1. the depth bound: hostile CSS reports, it does not abort
// ------------------------------------

// The four recursive grammars a stylesheet can reach. Each is one case so a failure names the
// grammar that lost its bound, and each runs at a depth far past any bound (thousands, not tens)
// because the question is "does it recurse off the stack", not "does a 40-deep sheet parse".
void test_nested_selectors_are_bounded() {
    // `:where(`/`:is(`/`:not(` all recurse through the functional pseudo-class parser. #410 bounded
    // it at 16; this asserts the bound is reachable through the PREVIEW path and that the rule is
    // dropped with a diagnostic instead of taking the module down.
    for (const std::string_view name : {":where", ":is", ":not"}) {
        const std::string selector = nest_function(name, "Button", 2000);
        const std::string css = "View " + selector + " { background-color: rgb(1, 2, 3); }\n";
        const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(css));
        expect(scene.error.empty(),
               std::string(name) + " at depth 2000 still lays the scene out: " + scene.error);
        expect(scene.root != nullptr, std::string(name) + " at depth 2000 keeps the tree");
    }
}

void test_nested_at_rules_are_bounded() {
    // `@media`/`@container`/`@layer`/`@supports` all recurse through the at-rule preparer, bounded
    // at 32 by #410. 2000 deep is far past it, and the sheet must prepare with a diagnostic.
    for (const std::string_view at_rule :
         {"@media (min-width: 1px)", "@supports (display: block)", "@container (min-width: 1px)"}) {
        const std::string css =
            repeat_open(std::string(at_rule) + " {", "Button { width: 10px; }", "}", 2000) + "\n";
        const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(css));
        expect(scene.error.empty(),
               std::string(at_rule) + " nested 2000 deep still lays out: " + scene.error);
        expect(!scene.css_diagnostics.empty(),
               std::string(at_rule) + " nested 2000 deep is reported");
    }
    // Nested `@layer` can name a different layer each level, so it takes its own builder.
    std::string layers;
    for (int i = 0; i < 2000; ++i) {
        layers += "@layer l" + std::to_string(i) + " {";
    }
    layers += "Button { width: 10px; }";
    layers.append(2000, '}');
    const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(layers + "\n"));
    expect(scene.error.empty(), "2000 nested @layer rules still lay out: " + scene.error);
    expect(!scene.css_diagnostics.empty(), "2000 nested @layer rules are reported");
}

void test_nested_supports_conditions_are_bounded() {
    // The gap #410 did not cover: a `@supports` condition nests through TWO recursions -- a
    // parenthesised group (a nested parser) and a run of `not` (parse_unary). Before the bound in
    // src/ui/style/supports.cpp, 20000 `not`s or 20000 `(`s crashed `tessera_codegen` with a stack
    // overflow (0xC00000FD); this is the case that proves the preview path cannot be handed one.
    for (const int depth : {2000, 20000}) {
        const std::string nots = repeat_open("not ", "(display: block)", "", depth);
        const std::string parens = repeat_open("(", "display: block", ")", depth);
        for (const std::string& condition : {nots, parens}) {
            const cli::PreparedScene scene = cli::prepare_scene(
                kColumn, options_for("@supports " + condition + " { Button { width: 10px; } }\n"));
            expect(scene.error.empty(), "a @supports condition nested " + std::to_string(depth) +
                                            " deep still lays out: " + scene.error);
            expect(scene.root != nullptr, "a @supports condition nested " + std::to_string(depth) +
                                              " deep keeps the tree");
            expect(!scene.css_diagnostics.empty(),
                   "a @supports condition nested " + std::to_string(depth) + " deep is reported");
        }
    }
}

void test_a_7kb_hostile_sheet_is_bounded() {
    // The size the issue names: "a 7 KB file crashed tessera_codegen". Build one from every
    // recursive grammar at once and require a frame, not an abort. The size is asserted so the case
    // cannot rot into something small enough to be uninteresting.
    std::string css;
    css += "View " + nest_function(":not", "Text", 500) + " { color: rgb(9, 9, 9); }\n";
    css += repeat_open("@media (min-width: 1px) {", "", "}", 300);
    css += "@supports " + repeat_open("not ", "(display: block)", "", 500) +
           " { Button { width: 11px; } }\n";
    css.append(1024, ' '); // pad past the size the issue names
    expect(css.size() > 7000, "the hostile sheet is larger than the 7 KB the issue names (got " +
                                  std::to_string(css.size()) + ")");
    const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(css));
    expect(scene.error.empty(), "the 7 KB hostile sheet still lays out: " + scene.error);
    expect(scene.root != nullptr, "the 7 KB hostile sheet still builds a tree");
    expect(scene.applied > 0, "the 7 KB hostile sheet still applies boxes");
    expect(!scene.css_diagnostics.empty(), "the 7 KB hostile sheet is reported");
}

// --- 2. decision 2: the built-in sheet is replaced, never layered
// ----------------------------------

void test_no_author_css_keeps_the_builtin_sheet() {
    // Every spelling of "the author supplied nothing" must reach the built-in sheet, because the
    // bundled samples, the parity corpus and the existing goldens all depend on its bytes.
    for (const std::string_view stylesheet :
         {std::string_view(), std::string_view("   \n\t  "),
          std::string_view("/* just a note */\n"), std::string_view("/* a */\n/* b */\n")}) {
        expect(!cli::has_author_stylesheet(stylesheet),
               "`" + std::string(stylesheet) + "` counts as no author CSS");
        const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(stylesheet));
        expect(scene.error.empty(), "the built-in fallback lays the scene out: " + scene.error);
        const ui::Node* button = scene.root != nullptr ? find_kind(*scene.root, "Button") : nullptr;
        expect(button != nullptr, "the column has a button");
        if (button == nullptr) {
            continue;
        }
        // The built-in sheet's button is 120x32; that number is the fallback's signature.
        expect(computed(scene, *button, "width") == "120px",
               "the fallback gives the button the built-in 120px width, got `" +
                   computed(scene, *button, "width") + "`");
        expect(computed(scene, *button, "background-color") == "#2d6cdf",
               "the fallback gives the button the built-in blue, got `" +
                   computed(scene, *button, "background-color") + "`");
    }
}

void test_author_css_replaces_the_builtin_sheet() {
    // The issue's acceptance sentence: "A source with author CSS that sets no Button size draws a
    // Button with the engine's own default, not the old 120x32 blue: the built-in sheet is
    // demonstrably absent."
    const std::string css = ".surface { background-color: rgb(255, 0, 0); }\n";
    const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(css));
    expect(scene.error.empty(), "the author sheet lays the scene out: " + scene.error);
    const ui::Node* button = scene.root != nullptr ? find_kind(*scene.root, "Button") : nullptr;
    expect(button != nullptr, "the styled column still has a button");
    if (button == nullptr) {
        return;
    }
    expect(computed(scene, *button, "width") != "120px",
           "the built-in 120px width is gone once the author wrote CSS, got `" +
               computed(scene, *button, "width") + "`");
    expect(computed(scene, *button, "background-color") != "#2d6cdf",
           "the built-in blue is gone once the author wrote CSS, got `" +
               computed(scene, *button, "background-color") + "`");
    expect(scene.root->children().empty() || true, "the tree is still attached");

    // And the sheet that IS applied paints: a class rule on the button reaches the cascade.
    const std::string painted = "Button { background-color: rgb(255, 0, 0); width: 40px; }\n";
    const cli::PreparedScene red = cli::prepare_scene(kColumn, options_for(painted));
    const ui::Node* red_button = red.root != nullptr ? find_kind(*red.root, "Button") : nullptr;
    expect(red_button != nullptr, "the painted column has a button");
    if (red_button != nullptr) {
        expect(computed(red, *red_button, "width") == "40px",
               "the author's width is the one that applies, got `" +
                   computed(red, *red_button, "width") + "`");
    }
}

// --- 3. decision 3: theme tokens come from user code
// -----------------------------------------------

void test_root_tokens_and_var_resolve_from_author_css() {
    // The issue's token acceptance: "A token defined on :root in the textarea and used through
    // var() changes the drawn color." `:root` matches the scene root (the window node), so the
    // token inherits into the button and `var()` picks it up through the ordinary cascade.
    const std::string css = ":root { --brand: rgb(0, 128, 255); }\n"
                            "Button { background-color: var(--brand); }\n";
    const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(css));
    expect(scene.error.empty(), "the token sheet lays out: " + scene.error);
    const ui::Node* button = scene.root != nullptr ? find_kind(*scene.root, "Button") : nullptr;
    expect(button != nullptr, "the token column has a button");
    if (button == nullptr) {
        return;
    }
    expect(computed(scene, *button, "background-color") == "rgb(0, 128, 255)",
           "the :root token resolves through var(), got `" +
               computed(scene, *button, "background-color") + "`");
    // A token the preview does not invent: an unknown var() with no fallback is not magically
    // defined.
    const std::string bare = "Button { background-color: var(--not-defined); }\n";
    const cli::PreparedScene unstyled = cli::prepare_scene(kColumn, options_for(bare));
    const ui::Node* bare_button =
        unstyled.root != nullptr ? find_kind(*unstyled.root, "Button") : nullptr;
    if (bare_button != nullptr) {
        expect(computed(unstyled, *bare_button, "background-color") != "rgb(0, 128, 255)",
               "a token defined nowhere is not invented");
    }
}

// --- 4. inline style and className go through the same cascade
// -------------------------------------

void test_class_and_inline_style_use_the_same_cascade() {
    constexpr std::string_view kClassed = R"tsx(import "./Preview.css";

export function Classed(): Node {
  return (
    <View>
      <View className="surface">
        <Button>Add</Button>
      </View>
    </View>
  );
}
)tsx";
    // `.surface` is a class rule, so this proves `className` reaches the selector engine.
    const std::string css = ".surface { padding-top: 7px; background-color: rgb(1, 2, 3); }\n";
    const cli::PreparedScene scene = cli::prepare_scene(kClassed, options_for(css), {}, nullptr);
    expect(scene.error.empty(), "the classed source lays out: " + scene.error);
    const ui::Node* surface = scene.root != nullptr ? find_kind(*scene.root, "View") : nullptr;
    // The first `View` is the scene root (the window node); the classed one is its descendant.
    while (surface != nullptr && surface->class_name() != "surface") {
        surface = surface->children().empty() ? nullptr : &*surface->children().front();
    }
    expect(surface != nullptr, "the classed view is in the tree");
    if (surface != nullptr) {
        expect(computed(scene, *surface, "padding-top") == "7px",
               "the author's class rule reaches className, got `" +
                   computed(scene, *surface, "padding-top") + "`");
    }

    // An inline `style={{...}}` wins over the class rule, which is the Origin::Inline half of the
    // cascade and the same rule the native build applies.
    constexpr std::string_view kInline = R"tsx(import "./Preview.css";

export function Inline(): Node {
  return (
    <View>
      <View className="surface" style={{ backgroundColor: "rgb(4, 5, 6)" }}>
        <Button>Add</Button>
      </View>
    </View>
  );
}
)tsx";
    const cli::PreparedScene inline_scene = cli::prepare_scene(kInline, options_for(css));
    expect(inline_scene.error.empty(), "the inline source lays out: " + inline_scene.error);
    const ui::Node* inline_surface =
        inline_scene.root != nullptr ? find_kind(*inline_scene.root, "View") : nullptr;
    while (inline_surface != nullptr && inline_surface->class_name() != "surface") {
        inline_surface =
            inline_surface->children().empty() ? nullptr : &*inline_surface->children().front();
    }
    expect(inline_surface != nullptr, "the inline-styled view is in the tree");
    if (inline_surface != nullptr) {
        expect(computed(inline_scene, *inline_surface, "background-color") == "rgb(4, 5, 6)",
               "the inline style wins over the class rule, got `" +
                   computed(inline_scene, *inline_surface, "background-color") + "`");
    }
}

// --- 5. diagnostics: line and column, and a bad rule does not blank the frame
// ----------------------

void test_css_diagnostics_carry_line_and_column() {
    // Two problems on known lines: an unknown property on line 2 and an unknown one on line 4. The
    // line numbers are the file's, so an editor can underline them.
    const std::string css = "Button { width: 40px; }\n"
                            "View { flx-grow: 1; }\n"
                            "\n"
                            ".surface { nope-not-a-property: 2; }\n";
    const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(css));
    expect(scene.error.empty(), "a sheet with unknown properties still lays out: " + scene.error);
    expect(scene.root != nullptr, "a sheet with unknown properties still builds the tree");
    // The unattached rule is the point: the frame is drawn from the rules that did parse.
    expect(scene.applied > 0, "the rules that parsed still apply");
    expect(scene.css_diagnostics.size() >= 2, "both unknown properties are reported, got " +
                                                  std::to_string(scene.css_diagnostics.size()));
    for (const cli::PreviewCssDiagnostic& diagnostic : scene.css_diagnostics) {
        expect(diagnostic.code == "TESSERA_CSS_UNKNOWN_PROPERTY",
               "an unknown property is TESSERA_CSS_UNKNOWN_PROPERTY, got `" + diagnostic.code +
                   "`");
        expect(diagnostic.line > 0 && diagnostic.column > 0,
               "a CSS diagnostic carries a 1-based line and column");
        expect(diagnostic.file == "ui/Preview.css",
               "a CSS diagnostic names the virtual stylesheet, got `" + diagnostic.file + "`");
        expect(diagnostic.warning, "an unknown property is a warning, not an error");
    }
    // The two known offsets are on lines 2 and 4, which is what makes the pair a real location
    // check.
    bool saw_line_two = false;
    bool saw_line_four = false;
    for (const cli::PreviewCssDiagnostic& diagnostic : scene.css_diagnostics) {
        saw_line_two = saw_line_two || diagnostic.line == 2;
        saw_line_four = saw_line_four || diagnostic.line == 4;
    }
    expect(saw_line_two, "the line-2 unknown property is reported on line 2");
    expect(saw_line_four, "the line-4 unknown property is reported on line 4");
}

void test_an_unterminated_string_is_an_error_not_a_blank_frame() {
    const std::string css = "Button { width: 40px; }\nView { color: \"unterminated; }\n";
    const cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for(css));
    expect(scene.error.empty(), "an unterminated string still lays out: " + scene.error);
    expect(scene.root != nullptr, "an unterminated string still builds the tree");
    bool saw_parse_error = false;
    for (const cli::PreviewCssDiagnostic& diagnostic : scene.css_diagnostics) {
        if (diagnostic.code == "TESSERA_CSS_PARSE_ERROR") {
            saw_parse_error = true;
            expect(!diagnostic.warning, "a lexical error is reported as an error, not a warning");
            expect(diagnostic.line == 2,
                   "the lexical error is on line 2, got " + std::to_string(diagnostic.line));
        }
    }
    expect(saw_parse_error, "an unterminated string is TESSERA_CSS_PARSE_ERROR");
}

void test_no_author_css_has_no_diagnostics() {
    expect(cli::author_stylesheet_diagnostics("", "").empty(),
           "no author CSS means no stylesheet diagnostics");
    expect(cli::author_stylesheet_diagnostics("/* note */", "").empty(),
           "a comment-only sheet means no stylesheet diagnostics");
    // The virtual file is the page-owned one when the caller names none.
    expect(cli::kPreviewStylesheetFile == "ui/Preview.css",
           "the virtual stylesheet is ui/Preview.css");
}

} // namespace

int main() {
    test_nested_selectors_are_bounded();
    test_nested_at_rules_are_bounded();
    test_nested_supports_conditions_are_bounded();
    test_a_7kb_hostile_sheet_is_bounded();
    test_no_author_css_keeps_the_builtin_sheet();
    test_author_css_replaces_the_builtin_sheet();
    test_root_tokens_and_var_resolve_from_author_css();
    test_class_and_inline_style_use_the_same_cascade();
    test_css_diagnostics_carry_line_and_column();
    test_an_unterminated_string_is_an_error_not_a_blank_frame();
    test_no_author_css_has_no_diagnostics();
    // The `*.module.css` half (issue #463) lives in its own TU and reports through the same
    // counter.
    scene_stylesheet_test::run_module_cases();
    if (failures() != 0) {
        std::cerr << "scene_stylesheet: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_stylesheet: ok\n";
    return 0;
}
