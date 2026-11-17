// SPDX-License-Identifier: MIT

#pragma once

#include "diagnostics_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The one conversion from the pipeline's own diagnostic types to the document record
// (docs/specs/compiler-diagnostics-v1.md). Both types already carry everything the record needs, so
// this unit only renames fields; it never re-derives a location, truncates a message, or rewords a
// code. `jsx::Diagnostic` has no file of its own (the front end parses one text at a time), so the
// caller passes the path it read the source from.
namespace tessera::compiler::codegen::cli {

[[nodiscard]] DiagnosticRecord to_record(const jsx::Diagnostic& diagnostic, std::string_view file);

[[nodiscard]] DiagnosticRecord to_record(const ir::IrDiagnostic& diagnostic);

// The span of a byte offset into a stylesheet. The stylesheet engine reports offsets only, so the
// line/column pair is derived here, once, and shared by the printed text and the document record so
// the two cannot disagree. A column counts bytes, which equals code points for the ASCII a selector
// name is made of; the `TESSERA_CSS_*` codes carry no multi-byte spans today.
//
// Inline because it is a pure function of its arguments and because the preview's stylesheet path
// (`tools/codegen/scene_stylesheet.cpp`) needs the same rule on the Wasm side, where this file's
// `to_record()` companions are not linked. One definition of "where is this CSS problem" for both.
[[nodiscard]] inline DiagnosticSpan css_span(std::string_view path, std::string_view text,
                                             std::size_t offset) {
    std::size_t line = 1;
    std::size_t column = 1;
    const std::size_t end = offset < text.size() ? offset : text.size();
    for (std::size_t i = 0; i < end; ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    DiagnosticSpan span;
    span.file = std::string(path);
    span.offset = static_cast<std::uint32_t>(end);
    span.length = 0;
    span.line = static_cast<std::uint32_t>(line);
    span.column = static_cast<std::uint32_t>(column);
    span.end_line = span.line;
    span.end_column = span.column;
    return span;
}

} // namespace tessera::compiler::codegen::cli
