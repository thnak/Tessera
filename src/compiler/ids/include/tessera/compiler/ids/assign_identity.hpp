// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <expected>
#include <string>
#include <string_view>

// Numeric identity for the IR (docs/specs/automation-ids-v1.md sections 2 and 3, "slice 10" of
// docs/design/jsx-ir-lowering.md): the compiler gives every component a `componentId` and every
// node a `uiId` from the project salt, the project-relative source path and the structural node
// path, so tools can address a node by a number that survives rebuilds.
namespace tessera::compiler::ids {

struct IdentityError {
    std::string message;
};

// Fills `component_id` of each component and `ui_id` of each node (all components, including
// conditional branches, error-boundary fallbacks and virtual item templates) that does not have
// one yet; ids already present, such as those of library components, are kept.
//
//   - `salt` is the `namespace` of tessera.json and must not be empty.
//   - A node with an explicit `automation_id` (decimal, non-zero) takes it as its `ui_id`.
//   - A node's path is `<Component>/root[0]/<Kind>[<index among siblings>]/...`; a fallback or an
//     item template adds `fallback`/`template` to the path. A literal `key` joins the hash input.
//   - Every node ends with `automation_id == ui_id`: the scene node's automation id is what
//     codegen emits and what snapshots report, and IR validation requires the two to agree.
//
// A duplicate explicit id, a zero or non-numeric one, and a 64-bit collision are errors; ids are
// never renumbered. On error the module is left unchanged.
[[nodiscard]] std::expected<void, IdentityError> assign_identity(ir::IrModule& module,
                                                                 std::string_view salt);

} // namespace tessera::compiler::ids
