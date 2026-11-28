// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/project_index.hpp"

#include <string>
#include <string_view>

namespace tessera::compiler::analysis {

// The `symbols` response result of the `analysis` service (docs/specs/studio-services-v1.md
// section 10) as deterministic JSON text (fixed member order, no whitespace). `file` selects one
// file; every file when empty.
std::string symbols_to_json(const ProjectIndex& index, std::optional<FileId> file);

// The `diagnostics` response result: `{ files: [{ file, diagnostics: [...] }] }` in file id order,
// one entry per file of the project (empty list when clean), or only `file`. A diagnostic is
// `{ code, severity, message, from, to }` (UTF-16, `to` exclusive) plus `related: { from, to,
// file, message }` when it has one. Deterministic text like `symbols_to_json`.
std::string diagnostics_to_json(const ProjectIndex& index, std::optional<FileId> file);

// Wire names of the enums (the JSON of every analysis response uses them) and a JSON string
// appender that escapes quotes, backslashes and control characters.
std::string_view wire_name(SymbolKind kind);
std::string_view wire_name(ReferenceKind kind);
std::string_view wire_name(Unresolved reason);
void append_json_string(std::string& out, std::string_view text);

// The `capabilities` result of the analysis service for this library: its feature names.
std::string analysis_capabilities_json();

} // namespace tessera::compiler::analysis
