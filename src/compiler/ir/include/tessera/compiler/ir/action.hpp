// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/expr.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tessera::compiler::ir {

// A typed parameter of a handler (docs/specs/ir-v1.md sections 2.1 and 6).
struct Param {
    std::string name;
    std::string type;
};

// `Focus`/`Blur` (issue #226) target a `ref::` and carry no value: they move keyboard focus to, or
// away from, the node the ref points at.
// `CallProp` (issue #286) calls a callback prop of the component itself: `target` is `prop::<name>`
// and `value` is the payload argument (absent for a payload-free `() => void` callback). A call
// through an unset callback is a no-op.
// `If` (ADR-066 item 2) is the conditional action: `guard` is the pure bool condition and the two
// lists are the branches. It is how `if`/`else` and an early `return` in a handler body lower.
// `If` (ADR-066 item 2) is the conditional action: `guard` is the pure bool condition and the two
// lists are the branches. It is how `if`/`else` and an early `return` in a handler body lower.
// `Dispatch` (ADR-066 item 4) is one `dispatch({ type: "x" })`: `target` is the reducer state's
// `state::` id and `action` the `type` literal. The generated listener indexes the state's dispatch
// table and calls the handler it names, so the component needs no new state kind.
// `CallService` (ADR-050 section 3.1 item 4, issue #177) is one `nav.push(route)` on a service the
// component requires: `target` is the requirement's `svc::<name>` id, `method` the manifest method
// and `args` its arguments in call order. The call is a side effect of a handler or an effect,
// never a value, so it has no `value`.
enum class ActionKind : std::uint8_t {
    Set,
    AnimSet,
    Focus,
    Blur,
    CallProp,
    If,
    Dispatch,
    CallService
};

// The spring driver of `v.set(x, { spring })`: an alternative to `duration`/`easing`. The defaults
// are the runtime's (`SpringConfig`: stiffness 180, damping 20, mass 1).
struct SpringTiming {
    double stiffness = 180.0;
    double damping = 20.0;
    double mass = 1.0;

    friend bool operator==(const SpringTiming&, const SpringTiming&) = default;
};

// One step of an inline handler or of a preview body.
struct Action {
    ActionKind kind = ActionKind::Set;
    // The fields an initializer spells in order: `{kind, target, value, animate}`. Every field
    // *after* `animate` has a default member initializer, so a positional init may stop at
    // `value` or `animate` and stay `-Wmissing-field-initializers`-free on GCC and Clang. A new
    // field therefore belongs after `animate`, with a default — a bare `std::string action;` here
    // is enough to warn and fail the Linux lanes, which is how this field first broke two of them.
    std::string target; // state::x or anim::x
    ExprPtr value;
    bool animate = true; // AnimSet only
    // ADR-066 item 4: the `type` literal of a `dispatch({ type: "x" })` (Dispatch only). Empty for
    // every other kind, and for a dispatch that carries no literal `type`, which the validator
    // rejects because the dispatch table is keyed by it.
    std::string action{};
    std::optional<double> duration_ms{};
    std::string easing{}; // CSS easing text, empty when absent
    // AnimSet only, exclusive with `duration_ms`/`easing` and with `animate: false`.
    std::optional<SpringTiming> spring{};
    // AnimSet only: the `handler::N` (an inline setter handler without parameters) that runs once
    // the value reaches this set's target. Empty means no callback. A later set drops it.
    std::string on_complete{};
    // Last so the positional `{kind, target, value, animate}` initializers keep working. A pure
    // bool expression: the action runs only when it holds (`e.key === "Escape" &&
    // setOpen(false)`, issue #236). Null means unconditional.
    ExprPtr guard{};
    // ADR-067 item 4: `startTransition(() => setQuery(q))` lowers to the same actions plus this
    // mark. The generated handler starts a transition under the resource's current epoch before it
    // assigns, so `isPending` is true while the change's fetch is in flight. Off by default: an
    // action without the mark is byte-identical to the IR before it.
    bool transition = false;
    // ADR-066 item 2: the branches of an `If` action. `then_actions` always runs when `guard`
    // holds; `else_actions` runs when it does not and is empty for a bare `if`. Both are lists
    // rather than single actions so a branch may hold several statements, and nesting is an `If`
    // inside either list. Empty for every other kind, so an action without them is byte-identical
    // to the IR before this field existed.
    std::vector<Action> then_actions{};
    std::vector<Action> else_actions{};
    // `CallService` only: the manifest method name and the argument expressions. Empty for every
    // other kind, so an action without them is byte-identical to the IR before they existed.
    std::string method{};
    std::vector<ExprPtr> args{};
};

} // namespace tessera::compiler::ir
