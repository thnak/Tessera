// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

// `get_ir` needs the lowered module, and `tools/codegen/diagnose.cpp` deliberately returns only the
// diagnostics document: its whole point is that the document is the only thing a page consumes.
// This unit therefore runs the same stages over the same in-memory source to produce the IR, and it
// only ever reports success -- the failure text always comes from `diagnose_source()`, so this
// adapter adds no second wording of a diagnostic and no second decision about which stage stops
// first (docs/specs/compiler-diagnostics-v1.md section 5).
namespace tessera::mcp {

// Lowers `text` as the single project-relative file `file` and returns the canonical IR JSON of
// docs/specs/ir-v1.md. Nullopt when the source does not lower into a module; the caller then has a
// non-empty diagnostics document to return instead.
[[nodiscard]] std::optional<std::string> lower_source_to_ir_json(std::string_view text,
                                                                 std::string_view file);

} // namespace tessera::mcp
