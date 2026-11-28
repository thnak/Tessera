// SPDX-License-Identifier: MIT

// Project-wide diagnostics (issue #568, ADR-060): an error in an imported file is reported against
// that file, parsing recovers after a syntax error, the order is stable, spans are UTF-16, and the
// worker's C ABI (analysis_entry.h) answers byte for byte what the library does.

#include "analysis_entry.h"
#include "jsx_test_util.hpp"
#include "tessera/compiler/analysis/project_index.hpp"
#include "tessera/compiler/analysis/symbol_json.hpp"

#include <string>
#include <vector>

using namespace tessera::compiler::analysis;
using jsx_test::check;
using jsx_test::check_eq;

namespace {

SourceFile make(FileId id, std::string path, std::string text) {
    return SourceFile{id, std::move(path), std::move(text)};
}

std::vector<Diagnostic> in_file(const std::vector<Diagnostic>& all, FileId file) {
    std::vector<Diagnostic> out;
    for (const Diagnostic& d : all) {
        if (d.span.file == file) {
            out.push_back(d);
        }
    }
    return out;
}

bool has_code(const std::vector<Diagnostic>& all, std::string_view code) {
    for (const Diagnostic& d : all) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

constexpr const char* kGood = "export function Card() { return <p />; }\n";
constexpr const char* kBroken = "export function Bad( {\n";

void test_error_in_imported_file_is_reported_there() {
    const ProjectIndex index = ProjectIndex::build(
        {make(0, "App.tsx",
              "import { Card } from \"./Card\";\nexport function App() { return <Card />; }\n"),
         make(1, "Card.tsx", kBroken)});
    const auto all = index.diagnostics();
    check(in_file(all, 0).empty(), "the importing file is clean");
    check(!in_file(all, 1).empty(), "the syntax error is against the imported file");
    check(index.diagnostics(0).empty(), "a single-file query keeps only that file");
    check(index.diagnostics(1).size() == in_file(all, 1).size(),
          "single-file query of the broken file");
}

void test_recovery_after_a_syntax_error() {
    const ProjectIndex index =
        ProjectIndex::build({make(0, "a.tsx", std::string(kBroken) + kGood)});
    check(!index.diagnostics().empty(), "the syntax error is reported");
    const FileIndex* file = index.file(0);
    bool card = false;
    for (const Symbol& symbol : file->symbols) {
        card = card || symbol.name == "Card";
    }
    check(card, "the item after the error is still indexed");
    for (const Diagnostic& d : index.diagnostics()) {
        check(d.span.to >= d.span.from, "span is ordered");
    }
}

void test_project_findings() {
    const ProjectIndex index = ProjectIndex::build(
        {make(0, "App.tsx",
              "import { Missing } from \"./Nope\";\nimport { Ghost } from \"./Card\";\n"
              "import s from \"./App.module.css\";\n"
              "export function App() { return <Missing />; }\n"),
         make(1, "Card.tsx", kGood),
         make(2, "Other.tsx", "export function Card() { return <b />; }\n")});
    const auto all = index.diagnostics();
    const auto app = in_file(all, 0);
    check(has_code(app, "TESSERA_ANALYSIS_IMPORT_FILE"), "missing file");
    check(has_code(app, "TESSERA_ANALYSIS_IMPORT_NAME"), "missing export");
    int css = 0;
    for (const Diagnostic& d : app) {
        css += d.severity == DiagnosticSeverity::Warning ? 1 : 0;
    }
    check(css == 1, "a missing stylesheet is a warning");
    const auto card = in_file(all, 1);
    const auto other = in_file(all, 2);
    check(card.size() == 1 && other.size() == 1, "each duplicate declaration is reported");
    if (card.size() == 1 && other.size() == 1) {
        check_eq(card[0].code, "TESSERA_ANALYSIS_DUPLICATE_COMPONENT", "duplicate code");
        check(card[0].related && card[0].related->file == 2, "related points at the other file");
        check(other[0].related && other[0].related->file == 1, "and back");
    }
}

void test_utf16_spans_and_stable_order() {
    // An astral character before the error counts as two units.
    const std::string text = std::string("// \xF0\x9F\x98\x80\n") + kBroken;
    const ProjectIndex one =
        ProjectIndex::build({make(0, "a.tsx", text), make(1, "b.tsx", kBroken)});
    const ProjectIndex two =
        ProjectIndex::build({make(1, "b.tsx", kBroken), make(0, "a.tsx", text)});
    check_eq(diagnostics_to_json(one, std::nullopt), diagnostics_to_json(two, std::nullopt),
             "input order does not change the answer");
    const auto first = in_file(one.diagnostics(), 0);
    const auto second = in_file(one.diagnostics(), 1);
    check(!first.empty() && !second.empty(), "both files report");
    if (!first.empty() && !second.empty()) {
        // "// " (3) + emoji (2 units, 4 bytes) + newline (1) shift the same error by 6 units.
        check(first[0].span.from == second[0].span.from + 6,
              "an astral character counts as two units");
    }
    const std::string json = diagnostics_to_json(one, std::nullopt);
    check_eq(json.substr(0, 33), "{\"files\":[{\"file\":0,\"diagnostics\"", "json shape");
}

void test_edits_equal_fresh_build() {
    ProjectIndex live = ProjectIndex::build(
        {make(0, "App.tsx", "import { Card } from \"./Card\";\n"), make(1, "Card.tsx", kBroken)});
    const std::vector<TextEdit> fix{{1, 0, 22, "export function Card() {}"}};
    check(live.apply_edits(fix), "edit applies");
    const ProjectIndex fresh =
        ProjectIndex::build({make(0, "App.tsx", "import { Card } from \"./Card\";\n"),
                             make(1, "Card.tsx", "export function Card() {}\n")});
    check_eq(diagnostics_to_json(live, std::nullopt), diagnostics_to_json(fresh, std::nullopt),
             "incremental equals fresh");
}

std::string via_abi(const std::vector<SourceFile>& files, const std::vector<TextEdit>& edits,
                    int file) {
    tessera_analysis_reset();
    for (const SourceFile& f : files) {
        tessera_analysis_set_file(static_cast<int>(f.id), f.path.c_str(), f.text.c_str());
    }
    tessera_analysis_commit_snapshot();
    for (const TextEdit& e : edits) {
        tessera_analysis_edit(static_cast<int>(e.file), static_cast<int>(e.offset),
                              static_cast<int>(e.length), e.new_text.c_str());
    }
    check(tessera_analysis_commit_edits() == 1, "abi edits commit");
    const char* out = tessera_analysis_query_json("diagnostics", file);
    const std::string text = out == nullptr ? "<null>" : out;
    tessera_free(out);
    return text;
}

void test_worker_abi_matches_library() {
    const std::vector<SourceFile> files{
        make(0, "App.tsx",
             "import { Card } from \"./Card\";\nimport x from \"./nope.module.css\";\n"),
        make(1, "Card.tsx", kBroken), make(2, "Dup.tsx", kGood), make(3, "Dup2.tsx", kGood)};
    const std::vector<TextEdit> edits{{0, 0, 0, "// edit\n"}};
    ProjectIndex direct = ProjectIndex::build(files);
    check(direct.apply_edits(edits), "direct edit");
    check_eq(via_abi(files, edits, -1), diagnostics_to_json(direct, std::nullopt),
             "abi equals library, all files");
    check_eq(via_abi(files, edits, 1), diagnostics_to_json(direct, 1),
             "abi equals library, one file");
    const char* caps = tessera_analysis_query_json("capabilities", -1);
    check_eq(std::string(caps), analysis_capabilities_json(), "capabilities");
    tessera_free(caps);
    check(tessera_analysis_query_json("nope", -1) == nullptr, "unknown query is null");
    const char* symbols = tessera_analysis_query_json("symbols", -1);
    check_eq(std::string(symbols), symbols_to_json(direct, std::nullopt),
             "symbols through the abi");
    tessera_free(symbols);
    tessera_analysis_edit(9, 0, 0, "x");
    check(tessera_analysis_commit_edits() == 0, "an edit of an unknown file is refused");
}

} // namespace

int main() {
    test_error_in_imported_file_is_reported_there();
    test_recovery_after_a_syntax_error();
    test_project_findings();
    test_utf16_spans_and_stable_order();
    test_edits_equal_fresh_build();
    test_worker_abi_matches_library();
    return jsx_test::failures == 0 ? 0 : 1;
}
