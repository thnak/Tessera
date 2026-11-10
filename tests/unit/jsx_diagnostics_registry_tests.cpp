// SPDX-License-Identifier: MIT

// Machine-checked diagnostic-code registry (docs/specs/jsx-subset-v1-jsx.md section 13.1).
//
// Three invariants keep one list honest:
//   1. Every `TESSERA_JSX_*` / `TESSERA_REACT_*` string literal the front end and the Tier-1 rules
//      emit is in the registry
//      (src/compiler/analyzer/include/tessera/compiler/jsx/diagnostic_registry.hpp).
//   2. Every registry code either has a fixture in jsx_diagnostics_cases.hpp or an explicit skip
//      entry with its owning stage; an uncovered code fails.
//   3. Every fixture reports exactly its code once, at the recorded span, with a bounded
//      one-line message. A skipped code may not be emitted by any stage, so moving a code into
//      this slice fails until its fixture replaces the skip.
//
// The fixture messages are also committed as a golden (tests/golden/jsx/diagnostics-registry.txt)
// and compared byte for byte, so a message edit is a visible diff.

#include "jsx_diagnostics_cases.hpp"
#include "jsx_test_util.hpp"
#include "tessera/compiler/jsx/diagnostic_registry.hpp"
#include "tessera/compiler/jsx/lexer.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_COMPILER_SOURCE_DIR
#error "TESSERA_COMPILER_SOURCE_DIR must be defined by the build"
#endif

#ifndef TESSERA_JSX_GOLDEN_DIR
#error "TESSERA_JSX_GOLDEN_DIR must be defined by the build"
#endif

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

namespace rules = tessera::compiler::rules;

struct Point {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

// Line/column computed here, independently of SourceMap, so the fixture pins the contract: one
// column per Unicode code point and LF as the only line break.
Point location_of(std::string_view source, std::uint32_t offset) {
    Point out;
    for (std::size_t i = 0; i < offset && i < source.size();) {
        if (source[i] == '\n') {
            ++out.line;
            out.column = 1;
            ++i;
            continue;
        }
        std::size_t length = 1;
        while (i + length < source.size() &&
               (static_cast<unsigned char>(source[i + length]) & 0xC0U) == 0x80U) {
            ++length;
        }
        i += length;
        ++out.column;
    }
    return out;
}

// Runs one fixture through the entry point it names. `where` names the fixture in every failure.
std::vector<Diagnostic> run_case(const jsx_registry::Case& test, const std::string& where) {
    if (test.entry == jsx_registry::Entry::kLex) {
        SourceMap map(test.source);
        DiagnosticSink sink(map, test.limits);
        Lexer lexer(test.source, sink, test.limits);
        while (!lexer.stopped()) {
            if (lexer.next(LexMode::Code).kind == TokenKind::Eof) {
                break;
            }
        }
        return sink.take();
    }
    switch (test.entry) {
    case jsx_registry::Entry::kExpression:
        return parse_expression(test.source, test.limits).diagnostics;
    case jsx_registry::Entry::kJsx:
        return parse_jsx_element(test.source, test.limits).diagnostics;
    case jsx_registry::Entry::kType:
        return parse_type(test.source, test.limits).diagnostics;
    case jsx_registry::Entry::kModule:
        return parse_module(test.source, test.limits).diagnostics;
    case jsx_registry::Entry::kRules: {
        // A rules fixture must be a clean module: an invalid component is skipped by the pass, so
        // a parse error would silently turn the expected report into no report at all.
        const ModuleParseResult parsed = parse_module(test.source, test.limits);
        check(parsed.ok(), "rules fixture parses cleanly: " + where);
        if (!parsed.root) {
            return {};
        }
        const SourceMap map(test.source);
        return rules::analyze_module(*parsed.root, map, test.limits).diagnostics;
    }
    case jsx_registry::Entry::kLex:
        break;
    }
    return {};
}

// Checks one fixture and records its message for the golden. `where` names the fixture in every
// failure so a broken case is findable without the line number of the log.
void check_case(const jsx_registry::Case& test, std::map<std::string, std::string>& messages) {
    const std::string where =
        "fixture `" + std::string(test.name) + "` (" + std::string(test.code) + ")";
    const std::vector<Diagnostic> diagnostics = run_case(test, where);
    if (diagnostics.size() != test.report_size) {
        std::cerr << "FAIL: report size for " << where << ": expected " << test.report_size
                  << ", got " << diagnostics.size() << "\n";
        ++failures;
    }
    std::size_t matches = 0;
    const Diagnostic* found = nullptr;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == test.code) {
            ++matches;
            found = found == nullptr ? &diagnostic : found;
        }
    }
    check(matches == 1, "the code is reported exactly once: " + where);
    if (found == nullptr) {
        return;
    }

    const ResolvedSpan& span = found->span;
    check_eq(std::to_string(span.span.offset), std::to_string(test.offset),
             "span byte offset: " + where);
    check_eq(std::to_string(span.span.length), std::to_string(test.length),
             "span byte length: " + where);
    check(span.span.end() <= test.source.size(), "span stays inside the fixture: " + where);
    const Point start = location_of(test.source, test.offset);
    const Point end = location_of(test.source, test.offset + test.length);
    check_eq(std::to_string(span.start.line) + ":" + std::to_string(span.start.column),
             std::to_string(start.line) + ":" + std::to_string(start.column),
             "resolved start line:column: " + where);
    check_eq(std::to_string(span.end.line) + ":" + std::to_string(span.end.column),
             std::to_string(end.line) + ":" + std::to_string(end.column),
             "resolved end line:column: " + where);
    check(span.start.line == test.line, "the fixture's expected line: " + where);

    const std::string message = found->message;
    check(!message.empty(), "non-empty message: " + where);
    check(message.size() <= kMaxDiagnosticTextBytes + 3, "bounded message: " + where);
    check(message.find('\n') == std::string::npos, "one-line message: " + where);
    const std::string prefix = std::to_string(span.start.line) + ":" +
                               std::to_string(span.start.column) + ": error[" +
                               std::string(test.code) + "]: ";
    check(format_diagnostic(*found).starts_with(prefix), "formatted prefix: " + where);
    messages[std::string(test.code)] = message;
}

void collect_quoted_codes(std::string_view text, std::set<std::string>& out) {
    constexpr std::string_view kMarker = "\"TESSERA_";
    for (std::size_t pos = text.find(kMarker); pos != std::string_view::npos;
         pos = text.find(kMarker, pos + kMarker.size())) {
        const std::size_t begin = pos + 1; // past the opening quote
        std::size_t end = begin;
        while (end < text.size()) {
            const char c = text[end];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
                break;
            }
            ++end;
        }
        const std::string_view code = text.substr(begin, end - begin);
        if (code.starts_with("TESSERA_JSX_") || code.starts_with("TESSERA_REACT_")) {
            out.emplace(code);
        }
    }
}

// Every code a source tree can report, read from the quoted literals so comments and
// documentation never count as an emission site. The registry header is skipped: it is the list
// being checked, not an emitter. The rules are scanned as well because they own the
// `TESSERA_REACT_*` checks that are not parser-level.
std::set<std::string> emitted_codes() {
    std::set<std::string> out;
    const std::filesystem::path root = TESSERA_COMPILER_SOURCE_DIR;
    for (const std::filesystem::path& directory :
         {root / "analyzer" / "src", root / "analyzer" / "include", root / "rules" / "src",
          root / "rules" / "include"}) {
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            ++failures;
            std::cerr << "FAIL: front-end source directory not readable: " << directory.string()
                      << "\n";
            continue;
        }
        for (std::filesystem::recursive_directory_iterator it(directory, error), last;
             !error && it != last; it.increment(error)) {
            const std::string name = it->path().filename().string();
            const std::string extension = it->path().extension().string();
            if (name == "diagnostic_registry.hpp" || (extension != ".cpp" && extension != ".hpp")) {
                continue;
            }
            std::ifstream in(it->path(), std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            collect_quoted_codes(text.str(), out);
        }
    }
    return out;
}

std::set<std::string> registry_codes() {
    std::set<std::string> out;
    for (const std::string_view code : kDiagnosticCodeRegistry) {
        out.emplace(code);
    }
    return out;
}

void report_failure(const std::string& message) {
    ++failures;
    std::cerr << "FAIL: " << message << "\n";
}

void check_registry(const std::set<std::string>& emitted, const std::set<std::string>& covered) {
    const std::set<std::string> registry = registry_codes();
    const std::string update =
        "adding or renaming a code requires updating the registry, the fixture or skip entry, "
        "and the golden";
    check(std::is_sorted(kDiagnosticCodeRegistry.begin(), kDiagnosticCodeRegistry.end()),
          "the registry list is sorted");
    check(std::adjacent_find(kDiagnosticCodeRegistry.begin(), kDiagnosticCodeRegistry.end()) ==
              kDiagnosticCodeRegistry.end(),
          "the registry list has no duplicate");
    std::set<std::string> skip_codes;
    for (const auto& skip : jsx_registry::kSkips) {
        if (!skip_codes.emplace(skip.code).second) {
            report_failure("duplicate skip entry for " + std::string(skip.code));
        }
        if (!registry.contains(std::string(skip.code))) {
            report_failure("skip entry `" + std::string(skip.code) + "` is not in the registry; " +
                           update);
        }
        if (covered.contains(std::string(skip.code))) {
            report_failure("`" + std::string(skip.code) +
                           "` has a fixture and a skip entry; remove the skip");
        }
        if (emitted.contains(std::string(skip.code))) {
            report_failure("skipped code `" + std::string(skip.code) +
                           "` is now emitted by a stage; add a fixture and remove the skip");
        }
    }
    for (const auto& code : emitted) {
        if (!registry.contains(code)) {
            report_failure("`" + code + "` is emitted by a stage but missing from the registry; " +
                           update);
        }
        if (!covered.contains(code)) {
            report_failure("uncovered emitted code `" + code +
                           "`: add a fixture to jsx_diagnostics_cases.hpp");
        }
    }
    for (const auto& code : covered) {
        if (!registry.contains(code)) {
            report_failure("fixture code `" + code + "` is missing from the registry; " + update);
        }
        if (!emitted.contains(code)) {
            report_failure("fixture code `" + code +
                           "` is not emitted by any front-end or rules source; the fixture or the "
                           "source scan is stale");
        }
    }
    for (const auto& code : registry) {
        if (covered.contains(code) || skip_codes.contains(code)) {
            continue;
        }
        report_failure("uncovered diagnostic code `" + code +
                       "`: add a fixture or an explicit skip entry; " + update);
    }
}

// A diagnostic that quotes input uses excerpt(): at most 32 bytes, cut on a code point boundary,
// plus "...". This is the message shape every fixture must preserve under hostile input.
void test_excerpt_cap() {
    check_eq(excerpt(std::string(40, 'a')), std::string(32, 'a') + "...",
             "excerpt caps at 32 bytes and marks the cut");
    const std::string split = std::string(31, 'a') + "\xC3\xA9"; // the last code point is 2 bytes
    check_eq(excerpt(split), std::string(31, 'a') + "...", "excerpt never splits a code point");
    const std::string name(40, 'q');
    const auto result = parse_jsx_element("<" + name + "/>");
    check_eq(std::string(first_diag(result).code), "TESSERA_JSX_SUBSET_HTML_TAG",
             "a long tag name still reports the tag code");
    const std::string message = first_diag(result).message;
    check(message.find(std::string(32, 'q') + "...") != std::string::npos,
          "the message quotes the capped excerpt");
    check(message.size() <= kMaxDiagnosticTextBytes + 3, "the capped message stays bounded");
}

// The sink is the last line of defense: even a call site that passes unbounded text cannot grow
// the report.
void test_sink_text_cap() {
    const std::string source = "a";
    SourceMap map(source);
    DiagnosticSink sink(map, Limits{});
    sink.error("TESSERA_JSX_LEX_INVALID_CHARACTER", std::string(400, 'm'), Span{0, 1},
               std::string(300, 's'));
    check(sink.diagnostics().size() == 1, "the sink keeps the capped diagnostic");
    const Diagnostic& diagnostic = sink.diagnostics().front();
    check(diagnostic.message.size() == kMaxDiagnosticTextBytes + 3,
          "the sink caps the message at 256 bytes plus ...");
    check(diagnostic.suggestion.size() == kMaxDiagnosticTextBytes + 3,
          "the sink caps the suggestion at 256 bytes plus ...");
    check(diagnostic.message.ends_with("..."), "the capped message is marked");
}

// The committed golden: code, fixture id and message per covered code, in lexicographic order.
// It is the reviewable record of every message, so a reworded message shows up as a diff even
// when all the fixture assertions still pass.
std::string build_golden(const std::map<std::string, std::string>& messages) {
    std::string out;
    out += "# Tessera diagnostic registry golden: one line per analyzer code with a fixture, in\n";
    out += "# lexicographic order. Format: code<TAB>fixture<TAB>message.\n";
    out += "# Compared byte for byte by tests/unit/jsx_diagnostics_registry_tests.cpp. Adding or\n";
    out += "# renaming a code requires updating this golden, diagnostic_registry.hpp and the\n";
    out +=
        "# fixture (or skip entry) in jsx_diagnostics_cases.hpp; `--update` rewrites this file.\n";
    std::vector<const jsx_registry::Case*> sorted;
    sorted.reserve(jsx_registry::kCases.size());
    for (const auto& test : jsx_registry::kCases) {
        sorted.push_back(&test);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* left, const auto* right) { return left->code < right->code; });
    for (const auto* test : sorted) {
        out += std::string(test->code) + '\t' + std::string(test->name) + '\t';
        const auto message = messages.find(std::string(test->code));
        out += message == messages.end() ? "<missing>" : message->second;
        out += '\n';
    }
    return out;
}

void compare_golden(const std::string& actual, bool update) {
    const std::string path = std::string(TESSERA_JSX_GOLDEN_DIR) + "/diagnostics-registry.txt";
    if (update) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << actual;
        std::cout << "rewrote " << path << "\n";
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const std::string golden = text.str();
    if (!in && golden.empty()) {
        report_failure("the diagnostic registry golden is not readable: " + path);
        return;
    }
    if (golden == actual) {
        return;
    }
    ++failures;
    std::cerr << "FAIL: the diagnostic registry golden is out of date\n"
              << "  path: " << path << "\n"
              << "  adding or renaming a code requires updating this golden\n"
              << "  --- committed golden ---\n"
              << golden << "  --- registry and fixtures ---\n"
              << actual;
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--update";
    std::map<std::string, std::string> messages;
    std::set<std::string> covered;
    std::set<std::string> names;
    for (const auto& test : jsx_registry::kCases) {
        if (!covered.emplace(test.code).second) {
            std::cerr << "FAIL: duplicate fixture for `" << test.code << "`\n";
            ++failures;
        }
        if (!names.emplace(test.name).second) {
            std::cerr << "FAIL: duplicate fixture id `" << test.name << "`\n";
            ++failures;
        }
        check_case(test, messages);
    }
    const std::set<std::string> emitted = emitted_codes();
    check_registry(emitted, covered);
    test_excerpt_cap();
    test_sink_text_cap();
    compare_golden(build_golden(messages), update);
    std::cout << kDiagnosticCodeRegistry.size() << " registry codes, "
              << jsx_registry::kCases.size() << " fixtures, " << jsx_registry::kSkips.size()
              << " skips, " << emitted.size() << " emitted codes\n";
    return finish("jsx diagnostics registry tests");
}
