// SPDX-License-Identifier: MIT

// ADR-064 item 7: the behavior surface of `<Component>.g.hpp` (docs/specs/behavior-hooks-v1.md
// section 9).
//
// A behavior is engine code, not generated code: the generated base owns one
// `tessera::ui::behavior::Instance` slot per behavior, attaches it after the target node exists,
// updates it when the hashed value options change, and detaches it in reverse order at unmount. The
// rules themselves live in `src/ui/behavior/`, so this TU is wiring only.
//
// Emission is conditional on `component.behaviors` being non-empty, which is what keeps every
// committed `.g.hpp` golden byte-identical (AGENTS.md: a golden moves only when its generator
// output really changed). A component that calls no behavior hook therefore generates no behavior
// code at all.

#pragma once

#include "codegen_internal.hpp"

#include <set>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {

// True when the component declares at least one behavior.
[[nodiscard]] bool uses_behaviors(const ir::Component& component);

// The engine headers the component's behaviors need, unique and sorted, in the `#include "..."`
// spelling (no quotes). Empty without behaviors.
[[nodiscard]] std::vector<std::string> behavior_headers(const ir::Component& component);

// Emits the private behavior slots: one `Instance` per behavior, named from its id. Emits nothing
// for a component without behaviors.
[[nodiscard]] bool emit_behavior_fields(CppWriter& writer, const ir::Component& component,
                                        std::string& error);

// Claims every generated behavior member name in the component's `names` set, so a TSX binding that
// collides with one is the usual `TESSERA_CODEGEN_UNSUPPORTED` collision instead of a
// redeclaration error.
[[nodiscard]] bool claim_behavior_names(const ir::Component& component,
                                        std::set<std::string>& names, std::string& error);

// Emits the behavior surface: one read accessor per output, the attach call in `build()`, the
// hashed update in the update pass, and the reverse-order detach in `unmount()`. Emits nothing for
// a component without behaviors.
[[nodiscard]] bool emit_behavior_accessors(CppWriter& writer, const ir::Component& component,
                                           std::string& error);

// Emits the public `attach_behaviors(tessera::ui::InputRouter&)` / `detach_behaviors()` pair the
// owner calls once the tree is built, plus the `refresh_behaviors()` the owner calls after an input
// dispatch. This is the seam that makes a behavior reachable at all: without it the generated base
// owns slots that nothing ever attaches (the defect #641 fixes).
//
// The router is the one service a behavior needs: it owns the scene's `FocusManager` and its
// `KeyBindingRegistry`, so a focus, keyboard or press behavior reads everything through it and
// never sees a window handle (ADR-020).
[[nodiscard]] bool emit_behavior_wiring(CppWriter& writer, const ir::Component& component,
                                        std::string& error);

} // namespace tessera::compiler::codegen::internal
