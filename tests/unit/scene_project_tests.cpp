// SPDX-License-Identifier: MIT
//
// A whole project through the preview's evaluator (`evaluate_project`).
//
// The studio used to compile one file and a page-owned stylesheet textarea. A real project is an
// entry component that imports other components and stylesheets, so these cases pin the rules that
// make the preview answer what a native build would:
//
//   * the program is what the ENTRY reaches: an unrelated file is neither parsed nor compiled, so a
//     half-typed file cannot blank the screen;
//   * the selected component is the entry's, not an imported building block;
//   * the stylesheets are the IMPORTED ones, in the web's import-graph order, and a sheet that sits
//     in the project but is never imported contributes nothing;
//   * a project with no entry, or a bad entry, or a broken import says so and names the file --
//     there is no default file to fall back to.

#include "diagnose.hpp"
#include "scene_evaluate.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;

int failures = 0;

void expect(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

constexpr std::string_view kApp = R"tsx(import "../styles/theme.css";
import { Home } from "./Home";

export function App(): Node {
  return (
    <View>
      <Home />
    </View>
  );
}
)tsx";

constexpr std::string_view kHome = R"tsx(import "./home.css";

export function Home(): Node {
  return (
    <View>
      <Text>Home</Text>
    </View>
  );
}
)tsx";

constexpr std::string_view kUnrelated = "export function Broken(): Node { return (";

constexpr std::string_view kTheme = "View { background: #fff; }\n";
constexpr std::string_view kHomeCss = "Text { color: #123456; }\n";
constexpr std::string_view kOrphan = "Button { color: red; }\n";

struct Project {
    std::array<cli::ProjectFile, 3> sources{{
        {"ui/App.tsx", kApp},
        {"ui/Home.tsx", kHome},
        {"ui/Unrelated.tsx", kUnrelated},
    }};
    std::array<cli::ProjectFile, 3> sheets{{
        {"styles/theme.css", kTheme},
        {"ui/home.css", kHomeCss},
        {"styles/orphan.css", kOrphan},
    }};

    [[nodiscard]] cli::ProjectInput input(std::string_view entry) const {
        return cli::ProjectInput{sources, sheets, entry};
    }
};

void test_the_entry_component_is_selected() {
    const Project project;
    const auto scene = cli::evaluate_project(project.input("ui/App.tsx"), "demo", {});
    expect(scene.has_value(), "the project evaluates");
    if (!scene) {
        return;
    }
    expect(scene->selected().name == "App", "the entry's component is selected, not Home");
}

void test_an_unreached_file_is_not_compiled() {
    // `ui/Unrelated.tsx` does not parse. The entry never imports it, so it must not matter.
    const Project project;
    const auto scene = cli::evaluate_project(project.input("ui/App.tsx"), "demo", {});
    expect(scene.has_value(), "an unparseable file the entry never reaches does not fail the run");
}

void test_sheets_are_the_imported_ones_in_import_order() {
    const Project project;
    const auto scene = cli::evaluate_project(project.input("ui/App.tsx"), "demo", {});
    if (!scene) {
        expect(false, "the project evaluates");
        return;
    }
    // App imports theme.css first, then Home, which imports home.css: depth-first, first import
    // wins.
    expect(scene->sheets.size() == 2, "exactly the imported sheets");
    if (scene->sheets.size() != 2) {
        return;
    }
    expect(scene->sheets[0].path == "styles/theme.css", "the entry's own import is first");
    expect(scene->sheets[1].path == "ui/home.css",
           "an imported file's sheet follows, relative to it");
    expect(scene->sheets[1].text == kHomeCss, "the sheet carries the project's bytes");
    for (const cli::SheetUse& sheet : scene->sheets) {
        expect(sheet.path != "styles/orphan.css", "a sheet nothing imports contributes nothing");
    }
}

void test_a_selected_component_must_be_in_the_entry() {
    const Project project;
    const auto scene = cli::evaluate_project(project.input("ui/App.tsx"), "demo", "Home");
    expect(!scene.has_value() && scene.error().code == cli::EvaluateErrorCode::NoComponent,
           "Home is imported, not declared by the entry, so it is not selectable from it");
}

void test_no_entry_is_an_error_and_not_a_default() {
    const Project project;
    const auto none = cli::evaluate_project(project.input(""), "demo", {});
    expect(!none.has_value() && none.error().code == cli::EvaluateErrorCode::NoEntry,
           "an empty entry is refused");
    const auto missing = cli::evaluate_project(project.input("ui/Gone.tsx"), "demo", {});
    expect(!missing.has_value() && missing.error().code == cli::EvaluateErrorCode::NoEntry,
           "an entry that is not a project file is refused");
    expect(!missing.has_value() && missing.error().detail.find("ui/Gone.tsx") != std::string::npos,
           "the refusal names the entry");
}

void test_a_broken_reached_file_names_itself() {
    const Project project;
    const auto scene = cli::evaluate_project(project.input("ui/Unrelated.tsx"), "demo", {});
    expect(!scene.has_value() && scene.error().code == cli::EvaluateErrorCode::File &&
               scene.error().file == "ui/Unrelated.tsx",
           "a file that does not parse is named when the entry is that file");
}

void test_a_missing_import_names_the_importer() {
    constexpr std::string_view kLost = R"tsx(import { Nope } from "./Nope";

export function Lost(): Node {
  return (<View />);
}
)tsx";
    const std::array<cli::ProjectFile, 1> sources{{{"ui/Lost.tsx", kLost}}};
    const cli::ProjectInput input{sources, {}, "ui/Lost.tsx"};
    const auto scene = cli::evaluate_project(input, "demo", {});
    expect(!scene.has_value() && scene.error().code == cli::EvaluateErrorCode::File &&
               scene.error().file == "ui/Lost.tsx" &&
               scene.error().detail.find("./Nope") != std::string::npos,
           "an import that names no file is reported against the importing file");
}

void test_a_missing_stylesheet_names_the_importer() {
    const std::array<cli::ProjectFile, 2> sources{{{"ui/App.tsx", kApp}, {"ui/Home.tsx", kHome}}};
    const std::array<cli::ProjectFile, 1> sheets{
        {{"styles/theme.css", kTheme}}}; // home.css missing
    const cli::ProjectInput input{sources, sheets, "ui/App.tsx"};
    const auto scene = cli::evaluate_project(input, "demo", {});
    expect(!scene.has_value() && scene.error().file == "ui/Home.tsx",
           "a stylesheet that is not in the project fails the file that imports it");
}

void test_a_windows_path_is_the_same_file() {
    const std::array<cli::ProjectFile, 2> sources{{{"ui\\App.tsx", kApp}, {"ui\\Home.tsx", kHome}}};
    const std::array<cli::ProjectFile, 2> sheets{
        {{"styles\\theme.css", kTheme}, {"ui\\home.css", kHomeCss}}};
    const cli::ProjectInput input{sources, sheets, "ui/App.tsx"};
    const auto scene = cli::evaluate_project(input, "demo", {});
    expect(scene.has_value(),
           "backslash paths normalize to the POSIX form every id is hashed from");
}

// ---- the diagnostics document of a project ---------------------------------------------------

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

void test_a_clean_project_has_no_diagnostics() {
    const Project project;
    const std::string document = cli::diagnose_project(project.input("ui/App.tsx"));
    expect(contains(document, "\"diagnostics\": []"),
           "a clean project reports nothing: " + document);
    expect(contains(document, "ui/App.tsx"), "the entry is the document's primary file");
}

void test_a_parse_error_is_attributed_to_the_file_it_is_in() {
    constexpr std::string_view kBrokenHome = "export function Home(): Node { return (";
    const std::array<cli::ProjectFile, 2> sources{
        {{"ui/App.tsx", kApp}, {"ui/Home.tsx", kBrokenHome}}};
    const std::array<cli::ProjectFile, 2> sheets{
        {{"styles/theme.css", kTheme}, {"ui/home.css", kHomeCss}}};
    const std::string document =
        cli::diagnose_project(cli::ProjectInput{sources, sheets, "ui/App.tsx"});
    expect(contains(document, "\"file\": \"ui/Home.tsx\""),
           "a syntax error in an imported file names that file, not the entry: " + document);
}

void test_a_lowering_error_is_attributed_to_the_file_it_is_in() {
    // Parses and analyses, but `<Mystery />` names no component, so the lowering rejects it.
    constexpr std::string_view kBadHome = R"tsx(export function Home(): Node {
  return (<View><Mystery /></View>);
}
)tsx";
    const std::array<cli::ProjectFile, 2> sources{
        {{"ui/App.tsx", kApp}, {"ui/Home.tsx", kBadHome}}};
    const std::array<cli::ProjectFile, 2> sheets{
        {{"styles/theme.css", kTheme}, {"ui/home.css", kHomeCss}}};
    const std::string document =
        cli::diagnose_project(cli::ProjectInput{sources, sheets, "ui/App.tsx"});
    expect(!contains(document, "\"diagnostics\": []"),
           "the bad component is reported: " + document);
    expect(contains(document, "\"file\": \"ui/Home.tsx\""),
           "a lowering error is attributed to the imported file it is in: " + document);
}

void test_diagnosing_without_an_entry_says_so() {
    const Project project;
    expect(contains(cli::diagnose_project(project.input("")), "names no entry"),
           "no entry is reported");
    expect(contains(cli::diagnose_project(project.input("ui/Gone.tsx")), "ui/Gone.tsx"),
           "an entry that is not a file is named");
}

} // namespace

int main() {
    test_a_clean_project_has_no_diagnostics();
    test_a_parse_error_is_attributed_to_the_file_it_is_in();
    test_a_lowering_error_is_attributed_to_the_file_it_is_in();
    test_diagnosing_without_an_entry_says_so();
    test_the_entry_component_is_selected();
    test_an_unreached_file_is_not_compiled();
    test_sheets_are_the_imported_ones_in_import_order();
    test_a_selected_component_must_be_in_the_entry();
    test_no_entry_is_an_error_and_not_a_default();
    test_a_broken_reached_file_names_itself();
    test_a_missing_import_names_the_importer();
    test_a_missing_stylesheet_names_the_importer();
    test_a_windows_path_is_the_same_file();
    if (failures != 0) {
        std::cerr << "scene_project: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_project: ok\n";
    return 0;
}
