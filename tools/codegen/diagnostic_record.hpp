// SPDX-License-Identifier: MIT

#pragma once

#include "diagnostics_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"

#include <cstddef>
#include <string_view>

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
[[nodiscard]] DiagnosticSpan css_span(std::string_view path, std::string_view text,
                                      std::size_t offset);

} // namespace tessera::compiler::codegen::cli
