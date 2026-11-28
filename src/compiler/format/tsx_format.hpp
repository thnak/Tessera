// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/format/format.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/jsx/source_map.hpp"

#include <string>

// The TSX printer's two entry points, shared by format.cpp and recover.cpp (not public API).
namespace tessera::compiler::format {

// The printer proper: `source` is LF-normalised and has no BOM. A refusal carries diagnostics and
// no text.
FormatResult format_tsx_text(const std::string& source, const jsx::SourceMap& map,
                             const FormatOptions& options);

// Slice 4 (docs/specs/source-format-v1.md section 14): `parsed` has errors but a module tree. When
// every error sits inside a top-level item the parser gave up on, formats the items around those
// regions and keeps each region byte for byte; otherwise refuses like `format_tsx_text`.
FormatResult format_tsx_recovering(const std::string& source, const jsx::SourceMap& map,
                                   const jsx::ModuleParseResult& parsed,
                                   const FormatOptions& options);

} // namespace tessera::compiler::format
