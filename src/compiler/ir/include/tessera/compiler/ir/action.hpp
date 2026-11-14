// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/expr.hpp"

#include <optional>
#include <string>

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
enum class ActionKind { Set, AnimSet, Focus, Blur, CallProp };

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
    std::string target; // state::x or anim::x
    ExprPtr value;
    bool animate = true; // AnimSet only
    // AnimSet only, and only when `animate` is true: the literal timing options of
    // `v.set(x, { duration, easing })`. Absent means the runtime default (200 ms, ease-out).
    // Default member initializers keep `{kind, target, value, animate}` aggregate initializers
    // warning-free (-Wmissing-field-initializers).
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
};

} // namespace tessera::compiler::ir
