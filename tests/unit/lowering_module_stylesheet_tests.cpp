// SPDX-License-Identifier: MIT

// Issue #80: a side-effect `import "./x.css"` is resolved against the caller-provided stylesheet
// files. It fills `IrModule::stylesheets` with a stable hash, a missing file is an error at the
// import's span, and codegen warns that the stylesheet is not embedded yet.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

namespace {

using namespace lowering_module_test;

constexpr std::string_view kApp = "import \"./app.css\";\nfunction App() { return <View />; }\n";
constexpr std::string_view kCss = ".a { color: red; }\n";
// FNV-1a 64 of kCss, computed independently of the implementation.
constexpr std::string_view kCssHash = "fnv1a64:d65fad7694885a03";

lowering::ModuleLoweringResult lower_with(const FileFixture& fixture,
                                          std::span<const lowering::StylesheetFile> sheets) {
    const lowering::ModuleSource source = fixture.module_source();
    return lowering::lower_project(std::span<const lowering::ModuleSource>(&source, 1), "m", {},
                                   sheets);
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
    module.stylesheets.push_back(ir::StylesheetRef{"css::ui/app", "ui/app.css", "h"});
    const std::vector<std::string> warnings = tessera::compiler::codegen::codegen_warnings(module);
    check_eq(std::to_string(warnings.size()), "1", "one warning");
    check(warnings[0].starts_with("TESSERA_CG_STYLESHEET_NOT_EMBEDDED:"), "the code");
    check(warnings[0].find("ui/app.css") != std::string::npos, "names the file");
}

} // namespace

int main() {
    test_existing_file_fills_stylesheets();
    test_shared_and_nested_imports();
    test_missing_file_is_an_error_at_the_import();
    test_codegen_warns_for_each_stylesheet();
    return finish("lowering module stylesheet tests");
}
