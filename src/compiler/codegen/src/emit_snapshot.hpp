// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <string>

// Emission of `snapshot_state()` on a generated component: every `useState` and
// `useAnimatedValue` cell as a `tessera::ui::StateCell` named by its IR id, in declaration order.
// Tests and the preview-parity runner (issue #7) read the whole state through it, so no one needs
// reflection over the generated struct. Cells whose type has no scalar or list-of-scalar form
// (a node reference, an optional) are left out.
namespace tessera::compiler::codegen::internal {

// Declaration on the component struct.
void emit_snapshot_declaration(CppWriter& writer);

// Out-of-line definition.
void emit_snapshot_definition(CppWriter& writer, const ir::Component& component);

} // namespace tessera::compiler::codegen::internal
