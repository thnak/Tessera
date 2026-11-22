// SPDX-License-Identifier: MIT

#pragma once

#include "hook_lowering_internal.hpp"

#include <optional>
#include <vector>

// Internal seam of the handler-body statement forms (ADR-066 item 2). Not installed; only
// src/compiler/lowering includes it.
namespace tessera::compiler::lowering::internal {

// The four call forms a handler body may hold, defined in `handler_actions.cpp`. They are declared
// here because the control-flow walk in `handler_control.cpp` reaches them through
// `lower_call_action`, and a branch accepts exactly the calls the body does.
[[nodiscard]] std::optional<ir::Action> lower_setter_call(HookState& state, const jsx::Expr& call,
                                                          const jsx::Expr& callee,
                                                          ComponentNames& names);
[[nodiscard]] std::optional<ir::Action> lower_animated_set(HookState& state, const jsx::Expr& call,
                                                           const jsx::Expr& callee,
                                                           ComponentNames& names);
[[nodiscard]] std::optional<ir::Action> lower_ref_focus(HookState& state, const jsx::Expr& call,
                                                        const jsx::Expr& callee);
[[nodiscard]] std::optional<ir::Action> lower_callback_call(HookState& state, const jsx::Expr& call,
                                                            const jsx::Expr& callee,
                                                            const jsx::Expr& arrow);
// ADR-066 item 4: `dispatch({ type: "x" })`. The one argument is an object literal whose `type` is
// a string literal, because that is the dispatch table's key; anything else is refused rather than
// guessed at.
[[nodiscard]] std::optional<ir::Action> lower_dispatch_call(HookState& state, const jsx::Expr& call,
                                                            const jsx::Expr& callee);

// One call statement as an action: a setter, an animated-value `.set(...)`, a ref `focus`/`blur`,
// or a callback prop. Shared by the body walk and by an `if` branch, so both accept exactly the
// same calls. `arrow` is the handler the statement belongs to, which a callback call needs to check
// that its argument is the handler's own parameter.
[[nodiscard]] std::optional<ir::Action> lower_call_action(HookState& state,
                                                          const jsx::Expr& statement,
                                                          const jsx::Expr& arrow,
                                                          ComponentNames& names);

// One `if`/`else` statement as an `If` action: the condition is the `guard` and the two branches
// are the action lists. A nested `if` is an `If` inside one of them.
[[nodiscard]] std::optional<ir::Action> lower_if_action(HookState& state,
                                                        const jsx::Expr& statement,
                                                        const jsx::Expr& arrow,
                                                        ComponentNames& names);

// The statements of one `if`/`else` branch. A branch is a `Block`, except an `else if`, which nests
// an `If` directly; both shapes are handled.
[[nodiscard]] std::vector<ir::Action> lower_branch(HookState& state, const jsx::Expr& branch,
                                                   const jsx::Expr& arrow, ComponentNames& names);

// The first `count` statements of `arrow` as actions, defined in `handler_actions.cpp`. The preview
// body walk (in `handler_control.cpp`) reuses it, so a preview body and a handler body accept the
// same statements.
[[nodiscard]] std::vector<ir::Action> lower_statements(HookState& state, const jsx::Expr& arrow,
                                                       std::size_t count);

} // namespace tessera::compiler::lowering::internal
