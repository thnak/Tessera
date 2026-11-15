// SPDX-License-Identifier: MIT
//
// The document half of the dev-mode compiler-error overlay (issue #78 item 4): reading a
// compiler-diagnostics-v1 document, deciding whether it is a failure, and rendering its lines.
//
// The documents are the spec's own examples (docs/specs/compiler-diagnostics-v1.md section 2), so
// the test fails if the overlay ever assumes a field the spec does not require, and it covers the
// three rules the spec makes load-bearing: an empty array is a success, an unknown version is
// rejected, and a diagnostic without a span renders without a location.

#include "compiler_error_overlay_docs.hpp"
#include "tessera/ui/devtools/compiler_error_overlay.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace devtools = tessera::ui::devtools;
using tessera::test::compiler_error_overlay::check;
using tessera::test::compiler_error_overlay::kCleanDocument;
using tessera::test::compiler_error_overlay::kErrorDocument;
using tessera::test::compiler_error_overlay::kTwoErrorDocument;
using tessera::test::compiler_error_overlay::kWarningDocument;

void test_parse() {
    std::string error;
    const devtools::DiagnosticDocument document =
        devtools::parse_diagnostics(kErrorDocument, error);
    check(error.empty(), "the spec example parses");
    check(document.version == 1, "the version is read");
    check(document.file == "ui/Counter.tsx", "the primary file is read");
    check(document.diagnostics.size() == 1, "one diagnostic");
    if (document.diagnostics.empty()) {
        return;
    }
    const devtools::OverlayDiagnostic& first = document.diagnostics.front();
    check(first.code == "TESSERA_IR_UNSUPPORTED", "the code is read");
    check(first.severity == "error", "the severity is read");
    check(first.file.has_value() && *first.file == "ui/Counter.tsx", "the span file is read");
    check(first.line == 3 && first.column == 11, "the span line and column are read");
    check(!first.suggestion.empty(), "the suggestion is read");
}

void test_parse_rejects() {
    std::string error;
    // An unknown version is rejected, not reinterpreted (spec section 6).
    static_cast<void>(
        devtools::parse_diagnostics(R"({"diagnostics": [], "diagnosticsVersion": 2})", error));
    check(!error.empty(), "an unknown diagnosticsVersion is rejected");

    static_cast<void>(devtools::parse_diagnostics("not json at all", error));
    check(!error.empty(), "malformed JSON is rejected");

    static_cast<void>(devtools::parse_diagnostics(R"({"diagnostics": []})", error));
    check(!error.empty(), "a document without a version is rejected");

    static_cast<void>(devtools::parse_diagnostics(R"({"diagnosticsVersion": 1})", error));
    check(!error.empty(), "a document without a diagnostics array is rejected");

    // An empty array is a SUCCESS document, not a missing one (spec section 2).
    const devtools::DiagnosticDocument clean = devtools::parse_diagnostics(kCleanDocument, error);
    check(error.empty() && clean.version == 1 && clean.diagnostics.empty(),
          "an empty diagnostics array parses as a clean run");
    check(!devtools::has_errors(clean), "a clean run has no errors");
}

void test_has_errors() {
    std::string error;
    check(devtools::has_errors(devtools::parse_diagnostics(kErrorDocument, error)),
          "an error document is a failure");
    check(!devtools::has_errors(devtools::parse_diagnostics(kWarningDocument, error)),
          "a warnings-only document is not a failure");
}

void test_lines() {
    std::string error;
    const devtools::DiagnosticDocument document =
        devtools::parse_diagnostics(kErrorDocument, error);
    const std::vector<std::string> lines = devtools::overlay_lines(document);
    check(!lines.empty(), "a failed compile renders at least one line");
    if (lines.empty()) {
        return;
    }
    check(lines.front().find("compile failed") != std::string::npos,
          "the header says the compile failed");
    check(lines.front().find("ui/Counter.tsx") != std::string::npos,
          "the header names the primary file");
    bool has_code = false;
    bool has_location = false;
    bool has_hint = false;
    for (const std::string& line : lines) {
        has_code = has_code || line.find("TESSERA_IR_UNSUPPORTED") != std::string::npos;
        has_location = has_location || line == "ui/Counter.tsx:3:11";
        has_hint = has_hint || line.rfind("hint: ", 0) == 0;
    }
    check(has_code, "the code is rendered");
    check(has_location, "the location is rendered as file:line:column");
    check(has_hint, "the suggestion is rendered as a hint");

    // A diagnostic with no span renders without a location instead of inventing one.
    const devtools::DiagnosticDocument two = devtools::parse_diagnostics(kTwoErrorDocument, error);
    const std::vector<std::string> two_lines = devtools::overlay_lines(two);
    check(two_lines.size() > 3, "both diagnostics are rendered");
    std::size_t locations = 0;
    for (const std::string& line : two_lines) {
        locations += line.rfind("ui/App.tsx:", 0) == 0 ? 1 : 0;
    }
    check(locations == 1, "only the located diagnostic renders a location");

    // The block count is bounded.
    const std::vector<std::string> bounded = devtools::overlay_lines(two, 1);
    check(bounded.size() < two_lines.size(), "max_diagnostics bounds the rendered blocks");
    check(devtools::overlay_lines(devtools::parse_diagnostics(kCleanDocument, error)).empty(),
          "a clean document renders nothing");
}

} // namespace

int main() {
    test_parse();
    test_parse_rejects();
    test_has_errors();
    test_lines();
    if (tessera::test::compiler_error_overlay::failures != 0) {
        std::cerr << tessera::test::compiler_error_overlay::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "compiler error overlay document tests passed\n";
    return 0;
}
