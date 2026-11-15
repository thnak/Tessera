// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The machine-readable diagnostics document of the codegen CLI
// (docs/specs/compiler-diagnostics-v1.md). It exists because the preview page needs the same
// diagnostics the native CLI reports: a page can only list "diagnostics with spans" if the span
// survives the CLI's text formatting, and P1's acceptance is that the two agree
// (docs/design/reference-equantic-ui.md section 7.4).
//
// The record reuses the front end's and the IR's own span fields rather than introducing a third
// span model: `jsx::ResolvedSpan` (docs/specs/jsx-subset-v1-jsx.md section 13.2) and
// `ir::SourceSpan` (docs/specs/ir-v1.md section 8) both describe the same thing, and the JSON shape
// below is the `SourceSpan` + `Diagnostic` pair of docs/specs/designer-protocol-v1.md section 3.
namespace tessera::compiler::codegen::cli {

// One span, in the documented SourceSpan form (designer-protocol-v1.md section 3.1):
// project-relative POSIX `file`, byte `offset`/`length` in the UTF-8 source, and 1-based
// `line`/`column` pairs whose columns count code points. A record with no location leaves every
// field at its default and is written without a `span` member.
struct DiagnosticSpan {
    std::string file;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t end_line = 0;
    std::uint32_t end_column = 0;

    // True when the record carries a real source location (a setup/usage error does not).
    [[nodiscard]] bool has_location() const {
        return !file.empty() && line != 0;
    }
};

// A second location that explains the first, for example the opening tag of a mismatched pair.
struct DiagnosticRelated {
    DiagnosticSpan span;
    std::string message;
};

// One diagnostic, in the shape of docs/specs/jsx-subset-v1-jsx.md section 13.1. `severity` is the
// documented lowercase spelling (`error`/`warning`), not the enum, so the JSON is the spec's word.
struct DiagnosticRecord {
    std::string code; // TESSERA_* stable code; empty only for a message the CLI could not classify
    std::string severity = "error";
    std::string message;
    DiagnosticSpan span;
    std::string suggestion;                 // omitted from the JSON when empty
    std::vector<DiagnosticRelated> related; // omitted when empty
};

// Bumped only for a breaking change to the shape; adding an optional member keeps the version, per
// docs/specs/versioning-v1.md section 2.
inline constexpr int kDiagnosticsVersion = 1;

// The whole document of one run: the version, the primary file the run was about, and the
// diagnostics in report order (source order, as the front end and the validator produce them).
[[nodiscard]] std::string write_diagnostics_json(std::string_view primary_file,
                                                 const std::vector<DiagnosticRecord>& diagnostics);

// Writes the document to `path`. Returns false and sets `error` when the file cannot be written;
// the caller turns that into the documented setup exit code (2), never a partial file.
[[nodiscard]] bool write_diagnostics_file(const std::string& path, std::string_view primary_file,
                                          const std::vector<DiagnosticRecord>& diagnostics,
                                          std::string& error);

} // namespace tessera::compiler::codegen::cli
