// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The compiler-owned source formatter (ADR-061, docs/specs/source-format-v1.md). One printer for
// the CLI, CI, the studio and an agent, so every caller gets the same bytes.
namespace tessera::compiler::format {

enum class FileKind : std::uint8_t {
    Tsx,  // slice 1
    Css,  // slice 3: any .css file
    Json, // slice 3: tessera.json
};

struct FormatOptions {
    int print_width = 100; // `format.printWidth` in tessera.json
    int indent_width = 2;  // `format.indentWidth`
};

// Why a file was not formatted. A refusal never carries partial output.
struct FormatDiagnostic {
    std::string code; // TESSERA_FMT_SYNTAX, TESSERA_FMT_COMMENT, TESSERA_FMT_UNSUPPORTED,
                      // TESSERA_FMT_VERIFY
    std::string message;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

struct FormatResult {
    std::string text; // the formatted file; empty when refused
    bool changed = false;
    std::vector<FormatDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const {
        return diagnostics.empty();
    }
};

// The file kind `tessera fmt` formats for a path: `.tsx`, `.css` and a file named `tessera.json`;
// anything else is not a source file of ours.
std::optional<FileKind> file_kind_for_path(std::string_view path);

// `format.printWidth` (40..400) and `format.indentWidth` (1..8) from the text of a tessera.json,
// applied over `base`. A missing, invalid or out-of-range value, or a file that does not parse,
// leaves the base value: options never make a format fail.
FormatOptions format_options_from_tessera_json(std::string_view text, FormatOptions base = {});

// Formats `text`. Deterministic, idempotent, meaning-preserving, and it refuses (with a
// diagnostic that names the cause) rather than drop or move a comment.
FormatResult format_source(FileKind kind, std::string_view text, const FormatOptions& options = {});

} // namespace tessera::compiler::format
