// SPDX-License-Identifier: MIT
//
// The preview's `*.module.css` sheet (issue #463, the preview half of the CLI's scoped blob).
//
// The bug this file exists to catch is a silent one: the lowering scopes `className={styles.card}`
// to `card_0b82a5`, but the preview used to compile the sheet's RAW text, so the compiled sheet
// defined `.card` and the tree carried `card_0b82a5`. Nothing matched, nothing reported an error,
// and the component painted unstyled. So the cases below assert both halves in one place -- the
// class name the lowered node carries AND the selectors the compiled sheet defines -- and rely on
// the engine's own selector index, not on substring searches, so "the sheet defines it" means what
// the cascade means by it.
//
// The scan is the compiler's (`tessera_css_module`), not a preview copy, so these cases also pin
// that the preview and `tessera_codegen` scope a file to the same name: `module_hash()` of
// `ui/Card.module.css` is `0b82a5`, the value `css_module_scan_tests` pins independently.

#include "scene_prepare.hpp"
#include "scene_stylesheet.hpp"
#include "scene_stylesheet_test_support.hpp"
#include "tessera/compiler/css_module/module_scan.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/selector_match.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace scene_stylesheet_test {
namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace css = tessera::compiler::css_module;
namespace style = tessera::ui::style;
namespace ui = tessera::ui;

// The module file, and the class name its path hashes to. Spelled out rather than computed here: a
// test that hashes its own expectation cannot notice a hash that changed under it.
constexpr std::string_view kModuleFile = "ui/Card.module.css";
constexpr std::string_view kScopedCard = "card_0b82a5";

// The source that carries a module binding, and the sheet it imports.
constexpr std::string_view kModuleSource = R"tsx(import styles from "./Card.module.css";

export function Card(): Node {
  return (
    <View>
      <View className={styles.card}>
        <Button>Add</Button>
      </View>
    </View>
  );
}
)tsx";

// A source with a plain class and a side-effect import of the module sheet (no class map bound):
// the sheet is scoped by its `.module.css` suffix, not by how it is imported.
constexpr std::string_view kPlainClassSource = R"tsx(import "./Card.module.css";

export function Column(): Node {
  return (
    <View>
      <View className="card">
        <Button>Add</Button>
      </View>
    </View>
  );
}
)tsx";

cli::ScenePreviewOptions options_for(std::string_view stylesheet, std::string_view stylesheet_file,
                                     std::string_view source_file = "ui/Column.tsx") {
    cli::ScenePreviewOptions options;
    options.file = source_file;
    options.salt = "preview";
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    options.stylesheet = stylesheet;
    options.stylesheet_file = stylesheet_file;
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

const ui::Node* find_class(const ui::Node& node, std::string_view name) {
    if (node.class_name() == name) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_class(*child, name)) {
            return found;
        }
    }
    return nullptr;
}

// The computed value of `property` through the sheet the preview laid the tree out with.
std::string computed(const cli::PreparedScene& scene, const ui::Node& node,
                     std::string_view property) {
    if (!scene.sheet.has_value()) {
        return {};
    }
    style::SceneStyleTree tree(*scene.root);
    const style::ComputedStyle computed_style = tree.compute(*scene.sheet, node);
    const std::string* value = computed_style.find(property);
    return value != nullptr ? *value : std::string();
}

// True when the COMPILED sheet defines a selector that reads `name` as a class. Walks the engine's
// selector index, the same structure the cascade matches against.
bool sheet_defines_class(const style::CompiledStyleSheet& sheet, std::string_view name) {
    for (const style::SelectorIndex::Entry& entry : sheet.index().entries()) {
        for (const style::CompoundSelector& compound : entry.selector.compounds) {
            for (const std::string& declared : compound.classes) {
                if (std::string_view(declared) == name) {
                    return true;
                }
            }
        }
    }
    return false;
}

// --- 1. the suffix decides what gets compiled
// -------------------------------------------------

void test_a_module_sheet_compiles_scoped_names() {
    const std::string css = ".card { padding-top: 7px; }\n";
    std::optional<css::ModuleScanError> error;
    const std::string text = cli::preview_stylesheet_text(css, kModuleFile, error);
    expect(!error.has_value(), "ui/Card.module.css scans");
    expect(text.find(kScopedCard) != std::string::npos,
           "the text compiled for a module sheet reads card_0b82a5, got `" + text + "`");

    const style::CompiledStyleSheet sheet =
        cli::compile_preview_stylesheet(text, cli::preview_media_context(320.0f, 240.0f, 1.0f));
    expect(sheet_defines_class(sheet, kScopedCard),
           "the compiled sheet defines the scoped class `" + std::string(kScopedCard) + "`");
    // The half that used to be true and must not be: a bare `.card` rule in a module sheet is
    // exactly the rule that silently matched nothing (it is not what the tree carries).
    expect(!sheet_defines_class(sheet, "card"),
           "the compiled sheet defines no bare `.card` selector for a module sheet");
}

void test_a_plain_sheet_is_compiled_as_written() {
    const std::string css = ".surface { padding-top: 7px; }\n";
    for (const std::string_view file :
         {std::string_view("ui/Preview.css"), std::string_view("ui/Card.css"),
          std::string_view("ui/Card.module.css.bak"), std::string_view()}) {
        std::optional<css::ModuleScanError> error;
        const std::string text = cli::preview_stylesheet_text(css, file, error);
        expect(!error.has_value(), "`" + std::string(file) + "` is not scanned");
        expect(text == css, "`" + std::string(file) + "` compiles the author's bytes unchanged");
    }
    // And the compiled sheet keeps the author's own selector, which is what every pre-#463 caller
    // depends on.
    std::optional<css::ModuleScanError> error;
    const std::string text = cli::preview_stylesheet_text(css, "ui/Preview.css", error);
    const style::CompiledStyleSheet sheet =
        cli::compile_preview_stylesheet(text, cli::preview_media_context(320.0f, 240.0f, 1.0f));
    expect(sheet_defines_class(sheet, "surface"), "a plain sheet keeps its `.surface` selector");
}

// --- 2. the two halves agree: that is the defect
// ------------------------------------------------------

void test_the_lowered_class_and_the_compiled_selector_agree() {
    const std::string css = ".card { padding-top: 7px; }\n";
    const cli::PreparedScene scene =
        cli::prepare_scene(kModuleSource, options_for(css, kModuleFile, "ui/Card.tsx"));
    expect(scene.error.empty(), "the module source lays out: " + scene.error);
    expect(scene.root != nullptr, "the module source builds a tree");
    if (scene.root == nullptr || !scene.sheet.has_value()) {
        return;
    }
    // The lowering's half: `styles.card` is folded to the scoped name.
    const ui::Node* card = find_class(*scene.root, kScopedCard);
    expect(card != nullptr, "the lowered className is `" + std::string(kScopedCard) + "`");
    if (card == nullptr) {
        return;
    }
    // The compiled sheet's half, from the same run: a selector with that exact class exists ...
    expect(sheet_defines_class(*scene.sheet, card->class_name()),
           "the compiled sheet defines a selector for the class the tree carries (`" +
               std::string(card->class_name()) + "`)");
    // ... and it is the rule that applies, which is the user-visible acceptance.
    expect(computed(scene, *card, "padding-top") == "7px",
           "the scoped module rule is what styles the node, got `" +
               computed(scene, *card, "padding-top") + "`");
    expect(!sheet_defines_class(*scene.sheet, "card"),
           "the raw local name is not a selector in the compiled sheet");
}

// --- 2b. the isolation acceptance, on the preview path
// The preview and the Wasm studio run this code. A module's `.title` styles the node that reads
// `styles.title` and not a plain `className="title"` beside it; compiled unscoped, it would style
// both.
constexpr std::string_view kIsolationSource = R"tsx(import styles from "./Card.module.css";

export function Card(): Node {
  return (
    <View>
      <View className={styles.title}>Scoped</View>
      <View className="title">Global</View>
    </View>
  );
}
)tsx";

void test_a_module_class_does_not_reach_a_global_class_of_the_same_name() {
    const std::string css = ".title { padding-top: 7px; }\n";
    const cli::PreparedScene scene =
        cli::prepare_scene(kIsolationSource, options_for(css, kModuleFile, "ui/Card.tsx"));
    expect(scene.error.empty(), "the isolation source lays out: " + scene.error);
    if (scene.root == nullptr || !scene.sheet.has_value()) {
        return;
    }
    const ui::Node* scoped = find_class(*scene.root, "title_0b82a5");
    const ui::Node* global = find_class(*scene.root, "title");
    expect(scoped != nullptr && global != nullptr,
           "the tree carries both the scoped and the plain class");
    if (scoped == nullptr || global == nullptr) {
        return;
    }
    expect(computed(scene, *scoped, "padding-top") == "7px",
           "the module rule styles the node that reads styles.title");
    expect(computed(scene, *global, "padding-top") != "7px",
           "the module rule does not reach a plain `title` class, got `" +
               computed(scene, *global, "padding-top") + "`");
}

// --- 3. a rejected module sheet reports, and paints nothing unscoped
// -----------------------------------

void test_a_rejected_module_sheet_reports_and_paints_nothing() {
    // `:global { ... }` is the block form the design note rejects, so the scan fails. The `.card`
    // rule before it is the anti-vacuity half: compiled as written, it WOULD style the node that
    // carries `className="card"`, which is precisely the unscoped paint this must not do.
    const std::string css = ".card { padding-top: 11px; }\n"
                            ":global { Button { width: 9px; } }\n";
    const cli::PreparedScene scene =
        cli::prepare_scene(kPlainClassSource, options_for(css, kModuleFile));
    expect(scene.error.empty(),
           "a sheet that cannot be scanned still lays the scene out: " + scene.error);
    expect(scene.root != nullptr, "a sheet that cannot be scanned still builds the tree");

    bool reported = false;
    for (const cli::PreviewCssDiagnostic& diagnostic : scene.css_diagnostics) {
        if (diagnostic.code != "TESSERA_CSS_MODULE_GLOBAL_BLOCK") {
            continue;
        }
        reported = true;
        expect(!diagnostic.warning, "a scan failure is an error, not a warning");
        expect(diagnostic.file == std::string(kModuleFile),
               "the scan failure names the stylesheet, got `" + diagnostic.file + "`");
        expect(diagnostic.line == 2,
               "the scan failure is on line 2, got " + std::to_string(diagnostic.line));
        expect(diagnostic.column == 1, "the scan failure is at the `:global`, got column " +
                                           std::to_string(diagnostic.column));
    }
    expect(reported, "the scan failure is reported as TESSERA_CSS_MODULE_GLOBAL_BLOCK");

    // Nothing from the sheet was painted: not the raw `.card` rule, and not the built-in fallback
    // either (the author did write CSS, so #390 decision 2 says it is replaced, not resurrected).
    const ui::Node* card = scene.root != nullptr ? find_class(*scene.root, "card") : nullptr;
    expect(card != nullptr, "the tree still carries the author's `card` class");
    if (card != nullptr) {
        expect(computed(scene, *card, "padding-top") != "11px",
               "the unscoped `.card` rule is not applied, got `" +
                   computed(scene, *card, "padding-top") + "`");
    }
    const ui::Node* button = scene.root != nullptr ? find_kind(*scene.root, "Button") : nullptr;
    if (button != nullptr) {
        expect(computed(scene, *button, "width") != "120px",
               "the built-in fallback sheet is not substituted for a rejected module sheet");
    }
}

} // namespace

void run_module_cases() {
    test_a_module_sheet_compiles_scoped_names();
    test_a_plain_sheet_is_compiled_as_written();
    test_the_lowered_class_and_the_compiled_selector_agree();
    test_a_module_class_does_not_reach_a_global_class_of_the_same_name();
    test_a_rejected_module_sheet_reports_and_paints_nothing();
}

} // namespace scene_stylesheet_test
