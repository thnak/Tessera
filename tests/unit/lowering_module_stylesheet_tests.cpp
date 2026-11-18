// SPDX-License-Identifier: MIT

// Issue #80: a side-effect `import "./x.css"` is resolved against the caller-provided stylesheet
// files. It fills `IrModule::stylesheets` with a stable hash, a missing file is an error at the
// import's span, and codegen warns that the stylesheet is not embedded yet.
//
// Issue #463 (slice 2c): a `*.module.css` import also scans the sheet's class map into the IR,
// `className={styles.x}` folds to the scoped name, an unknown or dynamic read is an error, and a
// class no TSX reads is a warning.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

namespace {

using namespace lowering_module_test;

constexpr std::string_view kApp = "import \"./app.css\";\nfunction App() { return <View />; }\n";
constexpr std::string_view kCss = ".a { color: red; }\n";
// FNV-1a 64 of kCss, computed independently of the implementation.
constexpr std::string_view kCssHash = "fnv1a64:d65fad7694885a03";

constexpr std::string_view kModuleApp =
    "import styles from \"./Card.module.css\";\n"
    "function App() { return <View className={styles.title} />; }\n";
constexpr std::string_view kModuleCss = ".title { color: red; }\n";

lowering::ModuleLoweringResult lower_with(const FileFixture& fixture,
                                          std::span<const lowering::StylesheetFile> sheets) {
    const lowering::ModuleSource source = fixture.module_source();
    return lowering::lower_project(std::span<const lowering::ModuleSource>(&source, 1), "m", {},
                                   sheets);
}

lowering::ModuleLoweringResult
lower_project_with(const Project& project, std::span<const lowering::StylesheetFile> sheets) {
    const std::vector<lowering::ModuleSource> list = project.sources();
    return lowering::lower_project(list, "m", {}, sheets);
}

// The `className` expression of a component root, or null when it has none.
const ir::Expr* class_name_of(const ir::Component& component) {
    const auto found = component.root.props.find("className");
    return found == component.root.props.end() ? nullptr : found->second.get();
}

std::string class_name_text(const ir::Component& component) {
    const ir::Expr* value = class_name_of(component);
    return value == nullptr ? std::string() : expr_text(value);
}

void test_existing_file_fills_stylesheets() {
    const auto fixture = parse_file("ui/App.tsx", std::string(kApp));
    const lowering::StylesheetFile sheet{"ui/app.css", kCss};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(result.has_value(), "the import lowers: " + first_message(result));
    check_eq(std::to_string(result->stylesheets.size()), "1", "one stylesheet");
    check_eq(result->stylesheets[0].id, "css::ui/app", "the id");
    check_eq(result->stylesheets[0].path, "ui/app.css", "the project-relative path");
    check_eq(result->stylesheets[0].hash, std::string(kCssHash), "the stable hash");

    ir::IrModule module;
    module.name = "m";
    module.stylesheets = result->stylesheets;
    const std::string json = ir::write_ir_json(module);
    check(json.find("\"stylesheets\": [\n    {\n      \"hash\": \"fnv1a64:d65fad7694885a03\",\n"
                    "      \"id\": \"css::ui/app\",\n      \"path\": \"ui/app.css\"\n    }\n  ]") !=
              std::string::npos,
          "the IR JSON lists the stylesheet: " + json);

    check(json.find("\"classes\"") == std::string::npos,
          "a plain `.css` sheet writes no `classes` key: " + json);

    const auto again = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check_eq(again->stylesheets[0].hash, result->stylesheets[0].hash, "the hash is deterministic");
}

void test_shared_and_nested_imports() {
    Project project;
    project.add(parse_file("ui/App.tsx", std::string(kApp)));
    project.add(parse_file("ui/sub/Card.tsx", "import \"../app.css\";\nimport \"./card.css\";\n"
                                              "function Card() { return <View />; }\n"));
    const lowering::StylesheetFile sheets[] = {{"ui/sub/card.css", ".c {}"}, {"ui/app.css", kCss}};
    const auto list = project.sources();
    const auto result = lowering::lower_project(list, "m", {}, sheets);
    check(result.has_value(), "the project lowers: " + first_message(result));
    check_eq(std::to_string(result->stylesheets.size()), "2", "a shared sheet appears once");
    check_eq(result->stylesheets[0].path, "ui/app.css", "sorted by path, first");
    check_eq(result->stylesheets[1].path, "ui/sub/card.css", "sorted by path, second");
}

void test_missing_file_is_an_error_at_the_import() {
    const auto fixture = parse_file("ui/App.tsx", std::string(kApp));
    const auto result = lower_with(*fixture, {});
    check(!result.has_value(), "a missing stylesheet is rejected");
    check_eq(first_code(result), "TESSERA_IR_STYLESHEET_MISSING", "the code");
    check(first_message(result).find("./app.css") != std::string::npos, "names the import");
    const jsx::Diagnostic& diagnostic = result.error().front();
    check_eq(std::to_string(diagnostic.span.start.line), "1", "the import line");
    check_eq(std::to_string(diagnostic.span.start.column), "1", "the import column");

    const auto module_result = lowering::lower_module(fixture->module_source(), "m");
    check(!module_result.has_value(), "lower_module has no stylesheet source either");
    check_eq(first_code(module_result), "TESSERA_IR_STYLESHEET_MISSING", "same code");
}

void test_codegen_warns_for_each_stylesheet() {
    ir::IrModule module;
    module.name = "m";
    check(tessera::compiler::codegen::codegen_warnings(module).empty(),
          "no stylesheet, no warning");
    module.stylesheets.push_back(ir::StylesheetRef{"css::ui/app", "ui/app.css", "h", {}});
    const std::vector<std::string> warnings = tessera::compiler::codegen::codegen_warnings(module);
    check_eq(std::to_string(warnings.size()), "1", "one warning");
    check(warnings[0].starts_with("TESSERA_CG_STYLESHEET_NOT_EMBEDDED:"), "the code");
    check(warnings[0].find("ui/app.css") != std::string::npos, "names the file");
}

// Issue #463: the module import records the scan's class map on the stylesheet reference, sorted by
// local name and unique, with the scoped names the scan produced.
void test_module_import_records_classes() {
    const auto fixture =
        parse_file("ui/App.tsx", "import styles from \"./Card.module.css\";\n"
                                 "function App() { return <View className={styles.a} />; }\n");
    const lowering::StylesheetFile sheet{
        "ui/Card.module.css", ".b { color: red; }\n.a { color: blue; }\n.b { color: green; }\n"};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(result.has_value(), "a module import lowers: " + first_message(result));
    check_eq(std::to_string(result->stylesheets.size()), "1", "one stylesheet");
    const ir::StylesheetRef& ref = result->stylesheets[0];
    check_eq(ref.id, "css::ui/Card.module", "the module id drops the extension only");
    check_eq(ref.path, "ui/Card.module.css", "the project-relative path");
    check(ref.hash.starts_with("fnv1a64:"), "the hash is still the file-bytes hash");
    check_eq(std::to_string(ref.classes.size()), "2", "duplicate classes collapse");
    check_eq(ref.classes[0].local, "a", "sorted by local, first");
    check_eq(ref.classes[0].scoped, "a_0b82a5", "the first scoped name");
    check_eq(ref.classes[1].local, "b", "sorted by local, second");
    check_eq(ref.classes[1].scoped, "b_0b82a5", "the second scoped name");

    ir::IrModule module;
    module.name = "m";
    module.stylesheets = result->stylesheets;
    const std::string json = ir::write_ir_json(module);
    check(json.find("\"classes\": [") != std::string::npos,
          "a module sheet writes `classes`: " + json);
    check(json.find("\"local\": \"a\"") != std::string::npos, "the authored name is written");
    check(json.find("\"scoped\": \"a_0b82a5\"") != std::string::npos, "the scoped name is written");
}

// Issue #463: a single `styles.<class>` folds to the scoped class literal.
void test_class_name_lowers_to_scoped_literal() {
    const auto fixture = parse_file("ui/App.tsx", std::string(kModuleApp));
    const lowering::StylesheetFile sheet{"ui/Card.module.css", kModuleCss};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(result.has_value(), "the component lowers: " + first_message(result));
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr, "App is present");
    if (app != nullptr) {
        check_eq(class_name_text(*app), "{\"kind\":\"literal\",\"value\":\"title_0b82a5\"}",
                 "className is the scoped literal");
    }
}

// Issue #463: a template of two class reads folds to one space-separated class string.
void test_template_class_name_folds() {
    const auto fixture = parse_file(
        "ui/App.tsx",
        "import styles from \"./Card.module.css\";\n"
        "function App() { return <View className={`${styles.card} ${styles.on}`} />; }\n");
    const lowering::StylesheetFile sheet{"ui/Card.module.css", ".card {}\n.on {}\n"};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(result.has_value(), "the template lowers: " + first_message(result));
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr, "App is present");
    if (app != nullptr) {
        check_eq(class_name_text(*app),
                 "{\"kind\":\"literal\",\"value\":\"card_0b82a5 on_0b82a5\"}",
                 "the template folded to the scoped classes");
    }
}

// Issue #463: `"foo " + styles.title` folds too, and a component reference accepts the same form.
void test_concatenation_and_component_reference_fold() {
    const auto fixture = parse_file(
        "ui/App.tsx", "import styles from \"./Card.module.css\";\n"
                      "function App() { return <View className={\"foo \" + styles.title} />; }\n");
    const lowering::StylesheetFile sheet{"ui/Card.module.css", kModuleCss};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(result.has_value(), "the concatenation lowers: " + first_message(result));
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr, "App is present");
    if (app != nullptr) {
        check_eq(class_name_text(*app), "{\"kind\":\"literal\",\"value\":\"foo title_0b82a5\"}",
                 "the concatenation folded to the scoped class");
    }

    Project project;
    project.add(parse_file("ui/Card.tsx", "function Card() { return <View />; }\n"));
    project.add(parse_file("ui/App.tsx",
                           "import { Card } from \"./Card\";\n"
                           "import styles from \"./Card.module.css\";\n"
                           "function App() { return <Card className={styles.title} />; }\n"));
    const auto reference =
        lower_project_with(project, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(reference.has_value(), "the component reference lowers: " + first_message(reference));
    const ir::Component* caller = find_component(*reference, "App");
    check(caller != nullptr, "App is present");
    if (caller != nullptr) {
        check_eq(class_name_text(*caller), "{\"kind\":\"literal\",\"value\":\"title_0b82a5\"}",
                 "a component reference folds the scoped class too");
    }
}

// Issue #463: spreading the class map into props is not a static class read either. The parser
// already refuses it (`TESSERA_JSX_SUBSET_SPREAD`, jsx-subset-v1.md section 10.2), so this pins the
// code the user sees rather than a lowering guard.
void test_spread_of_styles_is_rejected() {
    const auto fixture =
        parse_file_raw("ui/App.tsx", "import styles from \"./Card.module.css\";\n"
                                     "function App() { return <View {...styles} />; }\n");
    check(!fixture->parsed.ok(), "a spread of `styles` does not parse");
    check(codes(fixture->parsed).find("TESSERA_JSX_SUBSET_SPREAD") != std::string::npos,
          "the parser reports the spread: " + codes(fixture->parsed));
}

// Issue #463: two module bindings can be in scope in one file; each read resolves against the map
// of the file its own binding names.
void test_two_bindings_in_one_file() {
    const auto fixture = parse_file(
        "ui/App.tsx",
        "import styles from \"./Card.module.css\";\n"
        "import theme from \"./Other.module.css\";\n"
        "function App() { return <View className={`${styles.card} ${theme.title}`} />; }\n");
    const lowering::StylesheetFile sheets[] = {{"ui/Card.module.css", ".card {}\n"},
                                               {"ui/Other.module.css", ".title {}\n"}};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(sheets, 2));
    check(result.has_value(), "both bindings lower: " + first_message(result));
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr, "App is present");
    if (app != nullptr) {
        // module_hash("ui/Card.module.css") and module_hash("ui/Other.module.css").
        check_eq(class_name_text(*app),
                 "{\"kind\":\"literal\",\"value\":\"card_0b82a5 title_108e19\"}",
                 "each binding resolved against its own module");
    }
}

// Issue #463: a class the module does not declare is an error naming the file and the class.
void test_unknown_module_class_is_an_error() {
    const auto fixture = parse_file(
        "ui/App.tsx", "import styles from \"./Card.module.css\";\n"
                      "function App() { return <View className={styles.missing} />; }\n");
    const lowering::StylesheetFile sheet{"ui/Card.module.css", ".title { color: red; }\n"};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(!result.has_value(), "an unknown class is rejected");
    check_eq(first_code(result), "TESSERA_CSS_MODULE_UNKNOWN_CLASS", "the code");
    check(first_message(result).find("missing") != std::string::npos, "names the class");
    check(first_message(result).find("ui/Card.module.css") != std::string::npos, "names the file");
}

// Issue #463: a computed access is not a static class read. The parser refuses `[...]` itself
// (`TESSERA_JSX_SUBSET_MEMBER`) but still builds the `Member{text="[]"}` node the AST documents, so
// the lowering guard is exercised with that shape: a computed read must never resolve to nothing.
void test_computed_module_class_is_dynamic() {
    const auto fixture = parse_file("ui/App.tsx", std::string(kModuleApp));
    jsx::ModuleItem* component_item = nullptr;
    for (jsx::ModuleItem& item : fixture->parsed.root->items) {
        if (item.kind == jsx::ItemKind::Component) {
            component_item = &item;
        }
    }
    check(component_item != nullptr && component_item->returned != nullptr &&
              !component_item->returned->attributes.empty(),
          "the fixture has one component with a className");
    if (component_item == nullptr || component_item->returned == nullptr ||
        component_item->returned->attributes.empty()) {
        return;
    }
    jsx::Expr& value = *component_item->returned->attributes.front().expr;
    value.text = "[]";
    jsx::ExprPtr index = std::make_unique<jsx::Expr>();
    index->kind = jsx::ExprKind::String;
    index->text = "title";
    value.operands.push_back(std::move(index));
    fixture->analyzed = rules::analyze_module(*fixture->parsed.root, fixture->map);

    const lowering::StylesheetFile sheet{"ui/Card.module.css", kModuleCss};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(!result.has_value(), "a computed access is rejected");
    check_eq(first_code(result), "TESSERA_JSX_STYLES_DYNAMIC",
             "the code: " + first_message(result));
}

// Issue #463: a class that depends on state keeps both branches as scoped constants. The choice
// between them is the ordinary conditional expression; only the names were resolved at compile
// time.
void test_conditional_class_keeps_both_branches_scoped() {
    const auto fixture =
        parse_file("ui/App.tsx",
                   "import { useState } from \"tessera\";\n"
                   "import styles from \"./Card.module.css\";\n"
                   "function App() {\n"
                   "  const [on, setOn] = useState<bool>(false);\n"
                   "  return <View className={`${styles.card} ${on ? styles.active : \"\"}`} />;\n"
                   "}\n");
    const lowering::StylesheetFile sheet{"ui/Card.module.css", ".card {}\n.active {}\n"};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(result.has_value(), "a conditional module class lowers: " + first_message(result));
    if (!result.has_value()) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr, "App is present");
    if (app != nullptr) {
        const std::string text = class_name_text(*app);
        check(text.find("card_0b82a5") != std::string::npos, "the static part is scoped: " + text);
        check(text.find("active_0b82a5") != std::string::npos, "the branch is scoped: " + text);
        check(text.find("conditional") != std::string::npos,
              "the choice stays an expression: " + text);
        check(text.find("styles") == std::string::npos, "no read of `styles` survives: " + text);
    }
    check(result->warnings.empty(), "both classes count as used, so there is no warning");
}

// Issue #463: the conditional form still refuses a read that has no class to substitute.
void test_conditional_with_bare_styles_is_dynamic() {
    const auto fixture =
        parse_file("ui/App.tsx", "import { useState } from \"tessera\";\n"
                                 "import styles from \"./Card.module.css\";\n"
                                 "function App() {\n"
                                 "  const [on, setOn] = useState<bool>(false);\n"
                                 "  return <View className={on ? styles : \"x\"} />;\n"
                                 "}\n");
    const lowering::StylesheetFile sheet{"ui/Card.module.css", ".card {}\n"};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(!result.has_value(), "a bare `styles` in a branch is rejected");
    check_eq(first_code(result), "TESSERA_JSX_STYLES_DYNAMIC",
             "the code: " + first_message(result));
}

// Issue #463: this is the isolation proof. Two files declare `.title`; each component reads its
// own module, and the two scoped names differ because the module paths differ.
void test_two_modules_scope_the_same_class_differently() {
    Project project;
    project.add(parse_file("ui/A.tsx",
                           "import styles from \"./A.module.css\";\n"
                           "function A() { return <View className={styles.title} />; }\n"));
    project.add(parse_file("ui/B.tsx",
                           "import styles from \"./B.module.css\";\n"
                           "function B() { return <View className={styles.title} />; }\n"));
    const lowering::StylesheetFile sheets[] = {{"ui/A.module.css", ".title {}\n"},
                                               {"ui/B.module.css", ".title {}\n"}};
    const auto result = lower_project_with(project, sheets);
    check(result.has_value(), "both files lower: " + first_message(result));
    const ir::Component* a = find_component(*result, "A");
    const ir::Component* b = find_component(*result, "B");
    check(a != nullptr && b != nullptr, "both components are present");
    if (a != nullptr && b != nullptr) {
        const std::string first = class_name_text(*a);
        const std::string second = class_name_text(*b);
        check(first != second,
              "the two `.title` classes do not collide: " + first + " / " + second);
        // module_hash("ui/A.module.css") and module_hash("ui/B.module.css").
        check_eq(first, "{\"kind\":\"literal\",\"value\":\"title_4e93c6\"}", "the A scoped class");
        check_eq(second, "{\"kind\":\"literal\",\"value\":\"title_4556ad\"}", "the B scoped class");
    }
}

// Issue #463: a class no component of the project reads is a warning, not an error.
void test_unused_module_class_warns() {
    const auto fixture = parse_file("ui/App.tsx", std::string(kModuleApp));
    const lowering::StylesheetFile sheet{"ui/Card.module.css",
                                         ".title { color: red; }\n.spare { color: blue; }\n"};
    const auto result = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check(result.has_value(), "an unused class still lowers: " + first_message(result));
    check_eq(std::to_string(result->warnings.size()), "1", "one warning");
    if (!result->warnings.empty()) {
        const lowering::LoweringWarning& warning = result->warnings.front();
        check_eq(warning.file, "ui/App.tsx", "the warning names the importing file");
        check_eq(std::string(warning.diagnostic.code), "TESSERA_CSS_MODULE_UNUSED_CLASS",
                 "the warning code");
        check(warning.diagnostic.severity == jsx::Severity::Warning, "it is a warning");
        check(warning.diagnostic.message.find("spare") != std::string::npos, "names the class");
        check(warning.diagnostic.message.find("ui/Card.module.css") != std::string::npos,
              "names the file");
    }

    // Determinism: the same project reports the same warnings.
    const auto again = lower_with(*fixture, std::span<const lowering::StylesheetFile>(&sheet, 1));
    check_eq(std::to_string(again->warnings.size()), "1", "the warning is deterministic");
    check_eq(again->warnings.empty() ? std::string()
                                     : std::string(again->warnings.front().diagnostic.message),
             result->warnings.empty() ? std::string()
                                      : std::string(result->warnings.front().diagnostic.message),
             "the same warning message");
}

// Issue #463: two module paths whose 24-bit hashes collide share a scope, so the project is
// rejected and both files are named.
void test_hash_collision_is_an_error() {
    Project project;
    project.add(parse_file("ui/One.tsx",
                           "import styles from \"./mod4797.module.css\";\n"
                           "function One() { return <View className={styles.a} />; }\n"));
    project.add(parse_file("ui/Two.tsx",
                           "import styles from \"./mod29280.module.css\";\n"
                           "function Two() { return <View className={styles.a} />; }\n"));
    // FNV-1a 64 over both paths truncates to the same 24 bits (952058).
    const lowering::StylesheetFile sheets[] = {{"ui/mod4797.module.css", ".a { color: red; }\n"},
                                               {"ui/mod29280.module.css", ".a { color: red; }\n"}};
    const auto result = lower_project_with(project, sheets);
    check(!result.has_value(), "the colliding project is rejected");
    check_eq(first_code(result), "TESSERA_CSS_MODULE_HASH_COLLISION", "the code");
    check(first_message(result).find("ui/mod4797.module.css") != std::string::npos,
          "names the first path");
    check(first_message(result).find("ui/mod29280.module.css") != std::string::npos,
          "names the second path");
}

} // namespace

int main() {
    test_existing_file_fills_stylesheets();
    test_shared_and_nested_imports();
    test_missing_file_is_an_error_at_the_import();
    test_codegen_warns_for_each_stylesheet();
    test_module_import_records_classes();
    test_class_name_lowers_to_scoped_literal();
    test_template_class_name_folds();
    test_concatenation_and_component_reference_fold();
    test_spread_of_styles_is_rejected();
    test_unknown_module_class_is_an_error();
    test_computed_module_class_is_dynamic();
    test_conditional_class_keeps_both_branches_scoped();
    test_conditional_with_bare_styles_is_dynamic();
    test_two_modules_scope_the_same_class_differently();
    test_two_bindings_in_one_file();
    test_unused_module_class_warns();
    test_hash_collision_is_an_error();
    return finish("lowering module stylesheet tests");
}
