// SPDX-License-Identifier: MIT

// Prepare-rename and rename across files (issue #574, ADR-060): a component, a prop and a CSS class
// across files, refusals with a code, applying the edits then re-analysing adds no diagnostics, and
// the worker ABI.

#include "analysis_entry.h"
#include "jsx_test_util.hpp"
#include "tessera/compiler/analysis/rename.hpp"

#include <array>
#include <string>
#include <vector>

using namespace tessera::compiler::analysis;
using jsx_test::check;
using jsx_test::check_eq;

namespace {

const std::string kCard =
    "export function Card({ title, count }: { title: string; count: int }) {\n"
    "  return <p className={\"x\"}>{title}</p>;\n"
    "}\n";
const std::string kApp = "import { Card } from \"./Card\";\n"
                         "import styles from \"./App.module.css\";\n"
                         "export function App() {\n"
                         "  const [n, setN] = useState<int>(0);\n"
                         "  return <Card title=\"x\" count={n} className={styles.card} />;\n"
                         "}\n";
const std::string kOther =
    "import { Card } from \"./Card\";\n"
    "import s from \"./App.module.css\";\n"
    "export function Other() {\n  return <Card title=\"y\" count={1} className={s.card} />;\n}\n";
const std::string kCss =
    ".card { color: red; }\n.title { margin: 1px; }\n.card:hover { color: blue; }\n";

ProjectIndex project() {
    return ProjectIndex::build({{0, "src/App.tsx", kApp},
                                {1, "src/Card.tsx", kCard},
                                {2, "src/App.module.css", kCss},
                                {3, "src/Other.tsx", kOther}});
}

std::uint32_t at(const std::string& text, std::string_view needle, std::size_t skip = 0) {
    return static_cast<std::uint32_t>(text.find(needle) + skip);
}

std::string code_of(const RenameResult& r) {
    return r.refusal ? r.refusal->code : "";
}

// Applies the edits to a copy and returns the new index.
ProjectIndex applied(const ProjectIndex& index, const RenameResult& result) {
    ProjectIndex copy = index;
    check(copy.apply_edits(result.edits), "the edits apply in order");
    return copy;
}

void test_prepare() {
    const ProjectIndex index = project();
    const PrepareRename p = prepare_rename(index, 0, at(kApp, "<Card", 2));
    check(p.ok, "a component tag is renameable");
    check_eq(p.placeholder, "Card", "the placeholder is the current name");
    check_eq(p.kind, "component", "kind");
    check(p.range.file == 0 && p.range.from == at(kApp, "<Card", 1), "the range is the tag name");
    const PrepareRename none = prepare_rename(index, 0, at(kApp, "export function") + 1);
    check(!none.ok && none.refusal && none.refusal->code == "TESSERA_ANALYSIS_RENAME_NO_NAME",
          "no name");
    const PrepareRename tag = prepare_rename(index, 1, at(kCard, "<p") + 1);
    check(!tag.ok && tag.refusal->code == "TESSERA_ANALYSIS_RENAME_UNRESOLVED", "an intrinsic tag");
    const PrepareRename css = prepare_rename(index, 0, at(kApp, "styles.card", 8));
    check(css.ok && css.kind == "cssClass" && css.placeholder == "card", "a css class");
}

void test_rename_component_across_files() {
    const ProjectIndex index = project();
    const RenameResult r = rename_symbol(index, 0, at(kApp, "<Card", 2), "Tile");
    check(!r.refusal, "renamed");
    // Declaration, two imports, two tags.
    check(r.edits.size() == 5, "every use in every file");
    for (std::size_t i = 1; i < r.edits.size(); ++i) {
        const TextEdit& a = r.edits[i - 1];
        const TextEdit& b = r.edits[i];
        check(a.file < b.file || (a.file == b.file && a.offset > b.offset),
              "edits sorted, offsets descending");
    }
    const ProjectIndex after = applied(index, r);
    check(after.text(1)->find("function Tile(") != std::string_view::npos, "declaration renamed");
    check(after.text(3)->find("<Tile title") != std::string_view::npos, "other file renamed");
    check(after.diagnostics().size() == index.diagnostics().size(), "no new diagnostics");
    // The same rename from the declaration gives the same edits.
    const RenameResult from_decl = rename_symbol(index, 1, at(kCard, "Card(", 1), "Tile");
    check_eq(rename_to_json(index, 1, at(kCard, "Card(", 1), "Tile"),
             rename_to_json(index, 0, at(kApp, "<Card", 2), "Tile"),
             "the caret place does not matter");
    check(from_decl.edits.size() == 5, "from the declaration");
}

void test_rename_prop_across_files() {
    const ProjectIndex index = project();
    const RenameResult r = rename_symbol(index, 0, at(kApp, "count=") + 1, "total");
    check(!r.refusal, "prop renamed");
    const ProjectIndex after = applied(index, r);
    check(after.text(1)->find("{ title, total }") != std::string_view::npos,
          "the destructured prop");
    check(after.text(0)->find("total={n}") != std::string_view::npos, "the attribute in App");
    check(after.text(3)->find("total={1}") != std::string_view::npos, "the attribute in Other");
    check(after.diagnostics().size() == index.diagnostics().size(), "no new diagnostics");
    check(code_of(rename_symbol(index, 1, at(kCard, "count }"), "title")) ==
              "TESSERA_ANALYSIS_RENAME_COLLISION",
          "a sibling prop collides");
}

void test_rename_state_and_locals() {
    const ProjectIndex index = project();
    const RenameResult r = rename_symbol(index, 0, at(kApp, "n, setN"), "total");
    check(!r.refusal && r.edits.size() == 2, "state declaration and its use");
    check(code_of(rename_symbol(index, 0, at(kApp, "n, setN"), "App")) ==
              "TESSERA_ANALYSIS_RENAME_COLLISION",
          "a module-scope name is shadowed");
    check(code_of(rename_symbol(index, 0, at(kApp, "n, setN"), "setN")) ==
              "TESSERA_ANALYSIS_RENAME_COLLISION",
          "a sibling local");
}

void test_rename_css_class() {
    const ProjectIndex index = project();
    const RenameResult r = rename_symbol(index, 0, at(kApp, "styles.card", 8), "tile");
    check(!r.refusal, "css class renamed");
    check(r.edits.size() == 4, "two selectors and a use in each of two files");
    const ProjectIndex after = applied(index, r);
    check(after.text(2)->find(".tile { color") != std::string_view::npos, "the selector");
    check(after.text(2)->find(".tile:hover") != std::string_view::npos, "the second selector");
    check(after.text(3)->find("s.tile") != std::string_view::npos,
          "a different binding of the same file");
    check(after.text(2)->find(".title") != std::string_view::npos, "another class is untouched");
    check(code_of(rename_symbol(index, 0, at(kApp, "styles.card", 8), "title")) ==
              "TESSERA_ANALYSIS_RENAME_COLLISION",
          "an existing class collides");
    check(code_of(rename_symbol(index, 0, at(kApp, "styles.card", 8), "my-class")) ==
              "TESSERA_ANALYSIS_RENAME_INVALID_NAME",
          "a class that is not a member name");
    const ProjectIndex no_css =
        ProjectIndex::build({{0, "src/App.tsx", kApp}, {1, "src/Card.tsx", kCard}});
    check(code_of(rename_symbol(no_css, 0, at(kApp, "styles.card", 8), "tile")) ==
              "TESSERA_ANALYSIS_RENAME_EXTERNAL",
          "a style sheet outside the project");
}

void test_refusals() {
    const ProjectIndex index = project();
    const auto cardtag = at(kApp, "<Card", 2);
    check(code_of(rename_symbol(index, 0, cardtag, "App")) == "TESSERA_ANALYSIS_RENAME_COLLISION",
          "component collides");
    check(code_of(rename_symbol(index, 0, cardtag, "Other")) == "TESSERA_ANALYSIS_RENAME_COLLISION",
          "project-wide component");
    check(code_of(rename_symbol(index, 0, cardtag, "tile")) ==
              "TESSERA_ANALYSIS_RENAME_INVALID_NAME",
          "lower case component");
    check(code_of(rename_symbol(index, 0, cardtag, "Card")) == "TESSERA_ANALYSIS_RENAME_NO_CHANGE",
          "same name");
    check(code_of(rename_symbol(index, 0, cardtag, "Class")) == "",
          "a capitalised keyword look-alike is fine");
    check(code_of(rename_symbol(index, 0, at(kApp, "n, setN"), "class")) ==
              "TESSERA_ANALYSIS_RENAME_RESERVED",
          "keyword");
    check(code_of(rename_symbol(index, 0, at(kApp, "n, setN"), "2x")) ==
              "TESSERA_ANALYSIS_RENAME_INVALID_NAME",
          "digit first");
    check(code_of(rename_symbol(index, 0, at(kApp, "n, setN"), "")) ==
              "TESSERA_ANALYSIS_RENAME_INVALID_NAME",
          "empty");
    check(code_of(rename_symbol(index, 0, 0, "x")) != "", "a keyword is not a name");
    check(rename_symbol(index, 0, 0, "x").edits.empty(), "a refusal has no edits");
}

void test_package_alias_generated() {
    const std::string pkg =
        "import { Button } from \"ui-kit\";\nexport function A() { return <Button />; }\n";
    const ProjectIndex p = ProjectIndex::build({{0, "A.tsx", pkg}});
    check(code_of(rename_symbol(p, 0, at(pkg, "<Button", 2), "Btn")) ==
              "TESSERA_ANALYSIS_RENAME_EXTERNAL",
          "package import");
    check(code_of(rename_symbol(p, 0, at(pkg, "Button"), "Btn")) ==
              "TESSERA_ANALYSIS_RENAME_EXTERNAL",
          "package import binding");

    const std::string alias =
        "import { Card as C } from \"./Card\";\nexport function A() { return <C title=\"\" />; }\n";
    const ProjectIndex a = ProjectIndex::build({{0, "A.tsx", alias}, {1, "Card.tsx", kCard}});
    check(code_of(rename_symbol(a, 0, at(alias, "<C", 1), "D")) == "TESSERA_ANALYSIS_RENAME_ALIAS",
          "an aliased import");
    check(code_of(rename_symbol(a, 1, at(kCard, "Card(", 1), "Tile")) ==
              "TESSERA_ANALYSIS_RENAME_ALIAS",
          "from the origin too");

    const std::array<std::array<const char*, 2>, 3> generated{
        {{"./gen/Card", "src/gen/Card.tsx"},
         {"./Card.gen", "src/Card.gen.tsx"},
         {"../generated/Card", "generated/Card.tsx"}}};
    for (const auto& [specifier, path] : generated) {
        std::string app = kApp;
        app.replace(app.find("./Card"), 6, specifier);
        const ProjectIndex g = ProjectIndex::build({{0, "src/App.tsx", app}, {1, path, kCard}});
        check(code_of(rename_symbol(g, 0, at(app, "<Card", 2), "Tile")) ==
                  "TESSERA_ANALYSIS_RENAME_GENERATED",
              path);
    }
    const std::string marked = "// @generated by tessera\n" + kCard;
    const ProjectIndex g =
        ProjectIndex::build({{0, "src/App.tsx", kApp}, {1, "src/Card.tsx", marked}});
    check(code_of(rename_symbol(g, 0, at(kApp, "<Card", 2), "Tile")) ==
              "TESSERA_ANALYSIS_RENAME_GENERATED",
          "header");
}

void test_utf16_edits() {
    const std::string app = "// \xF0\x9F\x98\x80\nimport { Card } from \"./Card\";\nexport "
                            "function App() { return <Card title=\"\" count={1} />; }\n";
    const ProjectIndex index = ProjectIndex::build({{0, "App.tsx", app}, {1, "Card.tsx", kCard}});
    // The caret is in UTF-16 units: the emoji and its newline are 4 units before the import.
    const std::uint32_t tag = static_cast<std::uint32_t>(app.find("<Card") + 2 - 2);
    const RenameResult r = rename_symbol(index, 0, tag, "Tile");
    check(!r.refusal && r.edits.size() == 3, "renamed after an emoji");
    check(r.edits[0].file == 0, "sorted");
    const ProjectIndex after = applied(index, r);
    check(after.text(0)->find("<Tile title") != std::string_view::npos,
          "the right range was replaced");
    check(after.diagnostics().size() == index.diagnostics().size(), "no new diagnostics");
}

void test_abi() {
    const ProjectIndex index = project();
    tessera_analysis_reset();
    tessera_analysis_set_file(0, "src/App.tsx", kApp.c_str());
    tessera_analysis_set_file(1, "src/Card.tsx", kCard.c_str());
    tessera_analysis_set_file(2, "src/App.module.css", kCss.c_str());
    tessera_analysis_set_file(3, "src/Other.tsx", kOther.c_str());
    tessera_analysis_commit_snapshot();
    const auto offset = at(kApp, "<Card", 2);
    const auto grab = [](const char* text) {
        std::string out = text == nullptr ? "<null>" : text;
        tessera_free(text);
        return out;
    };
    check_eq(grab(tessera_analysis_rename_json(0, static_cast<int>(offset), "Tile")),
             rename_to_json(index, 0, offset, "Tile"), "abi rename");
    check_eq(grab(tessera_analysis_request_json("prepareRename", 0, static_cast<int>(offset), 0)),
             prepare_rename_to_json(index, 0, offset), "abi prepare");
    check_eq(grab(tessera_analysis_rename_json(0, -1, "Tile")), "<null>",
             "negative offset is null");
    const std::string refused = rename_to_json(index, 0, offset, "App");
    check(refused.starts_with(
              "{\"edits\":[],\"refusal\":{\"code\":\"TESSERA_ANALYSIS_RENAME_COLLISION\""),
          "refusal json");
    check(prepare_rename_to_json(index, 0, 0).starts_with("{\"prepare\":null,\"refusal\""),
          "prepare refusal json");
}

} // namespace

int main() {
    test_prepare();
    test_rename_component_across_files();
    test_rename_prop_across_files();
    test_rename_state_and_locals();
    test_rename_css_class();
    test_refusals();
    test_package_alias_generated();
    test_utf16_edits();
    test_abi();
    return jsx_test::failures == 0 ? 0 : 1;
}
