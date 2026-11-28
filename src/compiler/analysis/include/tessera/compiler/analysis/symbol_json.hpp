// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/project_index.hpp"

#include <string>

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

// The `capabilities` result of the analysis service for this library: its feature names.
std::string analysis_capabilities_json();

} // namespace tessera::compiler::analysis
