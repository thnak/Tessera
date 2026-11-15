// SPDX-License-Identifier: MIT

// ADR-040 generated base: `gen::<Component>Base<Self>` (docs/design/compiler.md section 7). The
// legacy emitters produce a header and a source for a plain struct; this seam derives the
// header-only CRTP template from them so both paths share one emitter set.
//
// The access section (data-member privacy, generated accessors) lives in
// `emit_base_accessors.hpp` so neither TU outgrows the 500-line hard limit.

#pragma once

#include "codegen_internal.hpp"
#include "emit_base_accessors.hpp"

#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {

// Builds `<Component>.g.hpp` from the legacy header/source texts of the same component.
//
// `markers`, when non-null, collects the TSX spans of the binding contract and makes the file
// carry `//@tessera-line <n>` markers around each `static_assert`; the caller resolves them into
// `#line` directives after the reshaping (issue #78). Null keeps the output byte-identical.
[[nodiscard]] bool emit_generated_base(const ir::Component& component,
                                       const std::vector<const ir::Binding*>& bindings,
                                       const std::string& legacy_header,
                                       const std::string& legacy_source, GeneratedFile& out,
                                       std::string& error, LineMapMarkers* markers = nullptr);

} // namespace tessera::compiler::codegen::internal
