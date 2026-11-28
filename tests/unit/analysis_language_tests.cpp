// SPDX-License-Identifier: MIT

// Definition, references and hover (issues #571 and #572, ADR-060): cross-file imports, props, CSS
// classes, UTF-16 offsets, hover signature/doc/diagnostics, and the worker ABI answering what the
// library does.

#include "analysis_entry.h"
#include "jsx_test_util.hpp"
#include "tessera/compiler/analysis/language.hpp"

#include <string>
#include <vector>

using namespace tessera::compiler::analysis;
using jsx_test::check;
using jsx_test::check_eq;

namespace {

SourceFile make(FileId id, std::string path, std::string text) {
    return SourceFile{id, std::move(path), std::move(text)};
}

// The UTF-16 offset of the n-th occurrence of `needle` (0-based) in a text whose only non-BMP
// characters are the emoji of the fixtures; callers pass ASCII-only prefixes unless noted.
void check_num(std::uint32_t actual, std::uint32_t expected, std::string_view what) {
    check(actual == expected, what);
}

std::uint32_t at(const std::string& text, std::string_view needle, int nth = 0) {
    std::size_t pos = 0;
    for (int i = 0; i <= nth; ++i) {
        pos = text.find(needle, i == 0 ? 0 : pos + 1);
    }
    return static_cast<std::uint32_t>(pos);
}

const std::string kCard =
    "/** A card with a title.\n * Second line. */\n"
    "export function Card({ title, count }: { title: string; count: int }) {\n"
    "  return <p>{title}</p>;\n"
    "}\n";

const std::string kApp = "import { Card } from \"./Card\";\n"
                         "import styles from \"./App.module.css\";\n"
                         "// The root.\n"
                         "export function App() {\n"
                         "  const [n, setN] = useState<int>(0);\n"
                         "  const bump = (): void => { setN(n + 1); };\n"
                         "  return <Card title=\"x\" count={n} className={styles.card} />;\n"
                         "}\n";

ProjectIndex project() {
    return ProjectIndex::build({make(0, "App.tsx", kApp), make(1, "Card.tsx", kCard)});
}

void test_definition_across_files() {
    const ProjectIndex index = project();
    const Definition tag = definition(index, 0, at(kApp, "<Card") + 2);
    check(tag.spans.size() == 1, "tag resolves");
    check(tag.spans[0].file == 1, "tag jumps to the other file");
    check_num(tag.spans[0].from, at(kCard, "Card({"), "tag lands on the declared name");
    const Definition imported = definition(index, 0, at(kApp, "Card") + 1);
    check(imported.spans.size() == 1 && imported.spans[0].file == 1,
          "the import binding follows to the origin");
    const Definition prop = definition(index, 0, at(kApp, "count="));
    check(prop.spans.size() == 1 && prop.spans[0].file == 1, "attribute jumps to the prop");
    check_num(prop.spans[0].from, at(kCard, "count }"), "attribute lands on the destructured prop");
    const Definition local = definition(index, 0, at(kApp, "n + 1"));
    check(local.spans.size() == 1 && local.spans[0].file == 0, "local state stays in its file");
    check_num(local.spans[0].from, at(kApp, "[n,") + 1, "state declaration");
}

void test_definition_unresolved_has_a_reason() {
    const ProjectIndex index = project();
    const Definition css = definition(index, 0, at(kApp, "card}") + 1);
    check(css.spans.empty() && css.reason == Unresolved::External, "css class is external");
    const Definition nothing = definition(index, 0, at(kApp, "// The root") + 1);
    check(nothing.spans.empty() && nothing.reason == Unresolved::None, "a comment names nothing");
    const Definition far = definition(index, 0, 100000);
    check(far.spans.empty(), "an offset past the end names nothing");
    const Definition unknown_file = definition(index, 9, 0);
    check(unknown_file.spans.empty(), "unknown file");
}

void test_references_across_files() {
    const ProjectIndex index = project();
    const ReferenceList from_decl = find_references(index, 1, at(kCard, "Card({") + 1, true);
    check_eq(from_decl.symbol, std::string("f1:Card"), "declaration id");
    check(from_decl.occurrences.size() == 3, "declaration, import binding and the tag");
    check(from_decl.occurrences[0].span.file == 0, "sorted by file");
    const ReferenceList from_use = find_references(index, 0, at(kApp, "<Card") + 2, false);
    check_eq(from_use.symbol, from_decl.symbol, "a use resolves to the same declaration");
    check(from_use.occurrences.size() == 1, "without the declarations: only the tag");
    const ReferenceList prop = find_references(index, 1, at(kCard, "count }"), true);
    check(prop.occurrences.size() == 2, "prop declaration plus the attribute in the other file");
    const ReferenceList local = find_references(index, 0, at(kApp, "n + 1"), true);
    check(local.occurrences.size() == 3, "state: declaration, two uses");
    const ReferenceList css = find_references(index, 0, at(kApp, "card}") + 1, true);
    check(css.symbol.empty() && css.reason == Unresolved::External, "css class has no symbol");
}

void test_references_use_utf16_offsets() {
    const std::string text = "export function A() { return <p />; }\n// \xF0\x9F\x98\x80\n"
                             "export function B() { return <A />; }\n";
    const ProjectIndex index = ProjectIndex::build({make(0, "a.tsx", text)});
    const ReferenceList list = find_references(index, 0, 16, true);
    check(list.occurrences.size() == 2, "declaration and use");
    // The emoji is 4 UTF-8 bytes and 2 UTF-16 units, so the use sits 2 units earlier than its byte.
    const std::uint32_t byte = static_cast<std::uint32_t>(text.find("<A />")) + 1;
    check_num(list.occurrences[1].span.from, byte - 2, "span in UTF-16 units");
    const Definition back = definition(index, 0, byte - 2);
    check(back.spans.size() == 1 && back.spans[0].from == 16, "caret in UTF-16 units resolves");
}

void test_hover() {
    const ProjectIndex index = project();
    const auto card = hover(index, 0, at(kApp, "<Card") + 2);
    check(card.has_value() && card->resolved, "component hover");
    check_eq(card->kind, std::string("component"), "kind");
    check_eq(card->signature,
             std::string("export function Card({ title, count }: { title: string; count: int })"),
             "signature drops the body");
    check_eq(card->doc, std::string("A card with a title.\nSecond line."), "doc comment");
    check(card->declaration && card->declaration->file == 1,
          "declaration points at the other file");
    check(card->exported, "exported");

    const auto prop = hover(index, 0, at(kApp, "count="));
    check(prop && prop->kind == "prop" && prop->container == "Card" && prop->signature.empty(),
          "prop hover names its component");

    const auto state = hover(index, 0, at(kApp, "n + 1"));
    check(state && state->kind == "state", "state hover");
    check_eq(state->signature, std::string("const [n, setN] = useState<int>(0);"),
             "state signature");
    check_eq(state->container, std::string("App"), "container");

    const auto app = hover(index, 0, at(kApp, "App()"));
    check(app && app->doc == "The root.", "line comment doc");

    const auto bump = hover(index, 0, at(kApp, "bump"));
    check(bump && bump->kind == "handler", "handler");
    check_eq(bump->signature, std::string("const bump = (): void =>"),
             "handler signature stops at the body");

    const auto css = hover(index, 0, at(kApp, "card}") + 1);
    check(css && !css->resolved && css->kind == "cssClass" && css->reason == Unresolved::External,
          "css class hover");
    check(!hover(index, 0, at(kApp, "// The root") + 1).has_value(), "nothing under the caret");
}

void test_hover_diagnostics() {
    const ProjectIndex index = ProjectIndex::build(
        {make(0, "App.tsx",
              "import { Ghost } from \"./Card\";\nexport function App() { return <Ghost />; }\n"),
         make(1, "Card.tsx", "export function Card() { return <p />; }\n")});
    const auto ghost = hover(index, 0, 10);
    check(ghost.has_value() && ghost->diagnostics.size() == 1,
          "the import diagnostic covers the name");
    check_eq(ghost->diagnostics[0].code, std::string("TESSERA_ANALYSIS_IMPORT_NAME"), "code");
    check(ghost->resolved && ghost->kind == "import",
          "an unresolved import answers with its own binding");
    const auto after = hover(index, 0, 31);
    check(!after || after->diagnostics.empty(), "outside the span there is none");

    const ProjectIndex broken = ProjectIndex::build({make(0, "b.tsx", "export function Bad( {\n")});
    bool any = false;
    for (std::uint32_t i = 0; i < 24 && !any; ++i) {
        const auto h = hover(broken, 0, i);
        any = h && !h->diagnostics.empty();
    }
    check(any, "a parser error shows on hover even with no name there");
}

void test_edit_updates_answers() {
    ProjectIndex index = project();
    const std::string insert = "// pad\n";
    check(index.apply_edits(std::vector<TextEdit>{{1, 0, 0, insert}}), "edit applies");
    const Definition tag = definition(index, 0, at(kApp, "<Card") + 2);
    check(tag.spans.size() == 1 && tag.spans[0].from == at(kCard, "Card({") + insert.size(),
          "definition follows the edit");
}

void test_json_and_abi() {
    const ProjectIndex index = project();
    const std::uint32_t tag = at(kApp, "<Card") + 2;
    check_eq(definition_to_json(index, 0, tag),
             "{\"definitions\":[{\"file\":1,\"from\":" + std::to_string(at(kCard, "Card({")) +
                 ",\"to\":" + std::to_string(at(kCard, "Card({") + 4) + "}]}",
             "definition json");
    check_eq(definition_to_json(index, 0, at(kApp, "card}") + 1),
             std::string("{\"definitions\":[],\"unresolved\":\"external\"}"), "unresolved json");
    check_eq(hover_to_json(index, 0, at(kApp, "// The root") + 1), std::string("{\"hover\":null}"),
             "empty hover json");

    tessera_analysis_reset();
    tessera_analysis_set_file(0, "App.tsx", kApp.c_str());
    tessera_analysis_set_file(1, "Card.tsx", kCard.c_str());
    tessera_analysis_commit_snapshot();
    const auto ask = [](const char* type, int file, int offset, int flag) {
        const char* text = tessera_analysis_request_json(type, file, offset, flag);
        std::string out = text == nullptr ? "<null>" : text;
        tessera_free(text);
        return out;
    };
    check_eq(ask("definition", 0, static_cast<int>(tag), 0), definition_to_json(index, 0, tag),
             "abi definition");
    check_eq(ask("references", 0, static_cast<int>(tag), 1),
             references_to_json(index, 0, tag, true), "abi references");
    check_eq(ask("hover", 0, static_cast<int>(tag), 0), hover_to_json(index, 0, tag), "abi hover");
    check_eq(ask("rename", 0, 0, 0), std::string("<null>"), "unknown type is null");
    check_eq(ask("hover", -1, 0, 0), std::string("<null>"), "negative file is null");
    check_eq(ask("hover", 0, -1, 0), std::string("<null>"), "negative offset is null");
}

} // namespace

int main() {
    test_definition_across_files();
    test_definition_unresolved_has_a_reason();
    test_references_across_files();
    test_references_use_utf16_offsets();
    test_hover();
    test_hover_diagnostics();
    test_edit_updates_answers();
    test_json_and_abi();
    return jsx_test::failures == 0 ? 0 : 1;
}
