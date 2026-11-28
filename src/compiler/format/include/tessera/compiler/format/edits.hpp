// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/format/format.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The edit-list form of the formatter (ADR-061 section 3, docs/specs/source-format-v1.md section
// 11).
namespace tessera::compiler::format {

// One replacement of the input text. `offset` and `length` are in UTF-16 code units of the input
// (the unit of a JavaScript string and of CodeMirror), `new_text` is UTF-8.
struct TextEdit {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::string new_text;
};

// A minimal edit list that turns `before` into `after`: a diff over identifier runs, whitespace
// runs and single punctuation characters. The edits are in the coordinates of `before`, ascending
// and non-overlapping, so applying them in any order gives `after`. Equal inputs give an empty
// list. Both texts must be valid UTF-8 (an invalid byte is treated as a letter).
std::vector<TextEdit> diff_text_edits(std::string_view before, std::string_view after);

struct FormatEditsResult {
    std::vector<TextEdit> edits; // empty when already canonical or refused
    std::vector<FormatDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const {
        return diagnostics.empty();
    }
};

// `format_source` followed by `diff_text_edits` against the input. A refusal carries the
// diagnostics and no edits, so the editor's text is left untouched.
FormatEditsResult format_edits(FileKind kind, std::string_view text,
                               const FormatOptions& options = {});

// The wire form shared by the CLI (`--edits-json`) and the Wasm entry, UTF-8 JSON:
// {"edits":[{"offset":0,"length":1,"newText":"x"}],"diagnostics":[{"code","message","line","column"}]}
std::string format_edits_json(FileKind kind, std::string_view text,
                              const FormatOptions& options = {});

} // namespace tessera::compiler::format
