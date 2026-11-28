// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/project_index.hpp"

#include <string>

namespace tessera::compiler::analysis {

// The `symbols` response result of the `analysis` service (docs/specs/studio-services-v1.md
// section 10) as deterministic JSON text (fixed member order, no whitespace). `file` selects one
// file; every file when empty.
std::string symbols_to_json(const ProjectIndex& index, std::optional<FileId> file);

// The `capabilities` result of the analysis service for this library: its feature names.
std::string analysis_capabilities_json();

} // namespace tessera::compiler::analysis
