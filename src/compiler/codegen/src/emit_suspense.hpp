// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_dynamic.hpp"
#include "emit_node_groups.hpp"

#include <string>
#include <vector>

// ADR-067 spike #661, off by default: the generated half of the pending UI.
//
// A `Suspense` boundary is a static node (ADR-067 item 2), so it owns no scene node of its own: the
// generated component owns both subtrees and attaches whichever one the boundary's model says is
// live. The model itself is `tessera::ui::pending::SuspenseBoundary`, header-only and shared with
// the IR evaluator, so the differential test compares wiring rather than two copies of the rule.
//
// A transition mark (ADR-067 item 4) is one `TransitionMark` member per component: a marked setter
// starts it before it assigns, so `is_pending()` is true from the moment the change is applied.
//
// Emission is conditional on the component having a `Suspense` node or a marked action, which is
// what keeps every committed `.g.hpp` golden byte-identical.
namespace tessera::compiler::codegen::internal {

// True when the component has a `Suspense` node or a transition-marked action.
[[nodiscard]] bool uses_pending(const ir::Component& component, const DynamicPlan& plan);

// The engine headers the pending surface needs, in `#include "..."` spelling (no quotes).
[[nodiscard]] std::vector<std::string> pending_headers(const ir::Component& component,
                                                       const DynamicPlan& plan);

// Emits the boundary slots, the fake resource and the transition mark. Emits nothing for a
// component without a pending surface.
void emit_pending_fields(CppWriter& writer, const ir::Component& component,
                         const DynamicPlan& plan);

// Emits the `apply_suspense_<n>()` declarations and the test seams that drive the fake resource.
void emit_pending_declarations(CppWriter& writer, const ir::Component& component,
                               const DynamicPlan& plan);

// Emits `apply_suspense_<n>()` (attach the live subtree) and the fake-resource seams.
[[nodiscard]] bool emit_pending_sources(CppWriter& writer, const ir::Component& component,
                                        const DynamicPlan& plan, std::string& error);

// Emits the two subtrees of one `Suspense` node into their ownership slots and attaches the live
// one. Called by emit_node in place of the normal child loop.
[[nodiscard]] bool emit_suspense_children(NodeEmitContext& context);

} // namespace tessera::compiler::codegen::internal
