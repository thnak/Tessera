// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_dynamic.hpp"

#include <string>
#include <vector>

// Emission of the scheduled-update surface of a generated component (issue #5,
// docs/design/compiler.md section 11): an `UpdateTicket` member, `attach_scheduler` /
// `detach_scheduler` / `schedule_update`, and one `set_<state>` setter per `useState` cell that
// assigns and, only when the value changed, marks the component dirty. The host's `UpdateScheduler`
// then runs `update()` once at its next flush, so no call site has to remember a manual `update()`.
namespace tessera::compiler::codegen::internal {

// Members and method declarations on the component struct.
bool emit_schedule_declarations(CppWriter& writer, const ir::Component& component,
                                std::string& error);

// Out-of-line definitions; owned component instances attach and detach after/before their owner so
// the flush order is parent before child. Per-item component instances follow the owner's
// scheduler: attach/detach reach every live item instance, and a late-created item joins through
// the stored pointer (docs/design/compiler-node-state.md section 3).
bool emit_schedule_definitions(CppWriter& writer, const ir::Component& component,
                               const std::vector<ComponentInstance>& instances,
                               const DynamicPlan& plan, std::string& error);

} // namespace tessera::compiler::codegen::internal
