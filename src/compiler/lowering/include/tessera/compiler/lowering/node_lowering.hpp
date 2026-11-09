// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/lowering/hook_lowering.hpp"

#include <expected>
#include <vector>

// Slice 7 of docs/design/jsx-ir-lowering.md: lower the returned JSX element of one analysed
// component to the IR node tree of docs/specs/ir-v1.md section 3, on top of the hook and binding
// data of slice 6. Component references (slice 9), conditionals and loops (slice 8), the type table
// (slice 4), custom elements (slice 11) and lowercase HTML aliases (slice 13) are rejected with
// TESSERA_IR_* diagnostics.
namespace tessera::compiler::lowering {

// One fully lowered component: the IR component plus the bindings that belong to the module rather
// than to the component (docs/specs/ir-v1.md sections 1 and 6).
struct LoweredComponent {
    ir::Component component;
    std::vector<ir::Binding> bindings; // sorted by id, from slice 6
};

using NodeLoweringErrors = std::vector<jsx::Diagnostic>;
using LoweredComponentResult = std::expected<LoweredComponent, NodeLoweringErrors>;

// Lowers hooks (slice 6) and then the returned element. `analysis` and `component` must match and
// the analysis must have succeeded, as for `lower_hooks`. Node ids are `node::N` in source
// pre-order from 1 (decision 2); `componentId` and `uiId` are left empty for slice 10, and
// `automationId` is filled from the attribute of the same name when present. Handler records for
// arrows written directly in JSX continue the `handler::N` numbering of the declared handlers, and
// `Ref::target` is filled for every ref attribute.
//
// A failing component yields diagnostics and no component data at all (first error only).
[[nodiscard]] LoweredComponentResult lower_component(const HookLoweringContext& context);

} // namespace tessera::compiler::lowering
