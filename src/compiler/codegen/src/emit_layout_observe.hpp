// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_events.hpp"

#include <string>
#include <string_view>
#include <vector>

// Emission of the layout-observation surface of a generated component (ADR-041,
// docs/design/layout-observation.md section 6.2): one `LayoutObservation` member per node with an
// `onSizeChange` event, a thunk that converts the delivered `SizeChange` to `SizeChangeEvent` and
// runs the handler, and `attach_layout_observers` / `detach_layout_observers`, which mirror
// `attach_scheduler` / `detach_scheduler` (owned component instances forward, parent first).
namespace tessera::compiler::codegen::internal {

// True when the component, or any component instance it owns (transitively), has a node with
// `onSizeChange`. Only such components get the observation members and methods, so every other
// generated file stays byte-identical.
[[nodiscard]] bool observes_size(const ir::Component& component, const ir::IrModule& module);

// `size_observation_<ordinal>_`: the member that owns one node's observation.
[[nodiscard]] std::string size_observation_member(unsigned ordinal);

// Members and method declarations on the component struct. Writes nothing when the component does
// not observe sizes.
void emit_size_observation_declarations(CppWriter& writer, const ir::Component& component,
                                        const ir::IrModule& module,
                                        const std::vector<EventAdapter>& adapters);

// Out-of-line attach/detach definitions.
void emit_size_observation_definitions(CppWriter& writer, const ir::Component& component,
                                       const ir::IrModule& module,
                                       const std::vector<EventAdapter>& adapters,
                                       const std::vector<ComponentInstance>& instances);

// The statements build() runs once the observed node `variable` exists: construct the observation
// and, when the component is already attached to a set, attach it there.
void emit_size_observation_binding(CppWriter& writer, std::string_view variable,
                                   const EventAdapter& adapter);

} // namespace tessera::compiler::codegen::internal
