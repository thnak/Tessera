// SPDX-License-Identifier: MIT

// Action validation (docs/specs/ir-v1.md section 2.1): one action's side effect, its guard, and the
// animation timing options it may carry. Split out of `validate_expr.cpp`, which had grown past the
// 500-line hard limit; the subject is the action, not the expression it assigns.
//
// The ADR-066 additions live here because they are all action *kinds*: `If` (item 2) is a
// conditional action with two branch lists, and `Dispatch` (item 4) names a reducer state and an
// action `type` the state's dispatch table must list.

#include "validate_internal.hpp"
#include "validate_record_detail.hpp"

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::ir {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool in_range(double value, double low, double high, bool low_inclusive) {
    return std::isfinite(value) && value <= high && (low_inclusive ? value >= low : value > low);
}

void validate_spring(ValidationContext& context, const SpringTiming& spring, const Action& action,
                     const SourceSpan& where) {
    if (action.duration_ms.has_value() || !action.easing.empty()) {
        context.report("TESSERA_IR_ANIMATION", "a spring cannot take a duration or easing", where);
    }
    if (!in_range(spring.stiffness, 0.0, 100000.0, false)) {
        context.report("TESSERA_IR_ANIMATION", "spring stiffness must be in (0, 100000]", where);
    }
    if (!in_range(spring.damping, 0.0, 10000.0, true)) {
        context.report("TESSERA_IR_ANIMATION", "spring damping must be in [0, 10000]", where);
    }
    if (!in_range(spring.mass, 0.0, 1000.0, false)) {
        context.report("TESSERA_IR_ANIMATION", "spring mass must be in (0, 1000]", where);
    }
}

// Timing options exist on `animSet` only, are meaningless when `animate` is false, and are bounded
// so a generated tween always finishes (ir-v1.md section 2.1).
void validate_animation_options(ValidationContext& context, const Action& action,
                                const SourceSpan& where) {
    const bool has_timing =
        action.duration_ms.has_value() || !action.easing.empty() || action.spring.has_value();
    if (!has_timing) {
        return;
    }
    if (action.kind != ActionKind::AnimSet) {
        context.report("TESSERA_IR_ANIMATION",
                       "only an `animSet` action takes duration, easing or spring", where);
        return;
    }
    if (!action.animate) {
        context.report("TESSERA_IR_ANIMATION",
                       "`animate: false` snaps, so it cannot take a duration, easing or spring",
                       where);
    }
    if (action.spring.has_value()) {
        validate_spring(context, *action.spring, action, where);
    }
    if (action.duration_ms.has_value() &&
        (!std::isfinite(*action.duration_ms) || *action.duration_ms < 0.0 ||
         *action.duration_ms > record_detail::kMaxAnimationMs)) {
        context.report("TESSERA_IR_ANIMATION", "duration must be 0..60000 ms", where);
    }
    if (!action.easing.empty() && !record_detail::valid_easing(action.easing)) {
        context.report("TESSERA_IR_ANIMATION",
                       "easing must be linear, ease, ease-in, ease-out, ease-in-out or "
                       "cubic-bezier(x1, y1, x2, y2) with x in [0, 1]",
                       where);
    }
}

// `onComplete` is the `handler::N` of a parameterless handler of the same component; only an
// `animSet` completes. A handler that completes into itself is allowed: the runtime bounds it.
void validate_on_complete(ValidationContext& context, const ComponentScope& scope,
                          const Action& action, const SourceSpan& where) {
    if (action.on_complete.empty()) {
        return;
    }
    if (action.kind != ActionKind::AnimSet) {
        context.report("TESSERA_IR_ANIMATION", "only an `animSet` action takes onComplete", where);
        return;
    }
    const Handler* target = nullptr;
    if (scope.component != nullptr) {
        for (const auto& handler : scope.component->handlers) {
            if (handler.id == action.on_complete) {
                target = &handler;
                break;
            }
        }
    }
    if (target == nullptr) {
        context.report("TESSERA_IR_UNRESOLVED",
                       "onComplete `" + action.on_complete + "` is not a handler of this component",
                       where);
    } else if (!target->params.empty()) {
        context.report("TESSERA_IR_ANIMATION",
                       "onComplete handler `" + action.on_complete + "` must take no parameters",
                       where);
    }
}

// ADR-066 item 4: whether `action` is one of the `type` literals `state_id`'s reducer handles.
bool has_dispatch_case(const ComponentScope& scope, const std::string& state_id,
                       const std::string& action) {
    const auto table = scope.dispatch.find(state_id);
    if (table == scope.dispatch.end()) {
        // Not a reducer state; the caller reports that, so this answers "no" without a second
        // diagnostic.
        return true;
    }
    return table->second.count(action) != 0;
}

} // namespace

void validate_actions(ValidationContext& context, const ComponentScope& scope,
                      const LocalNames& locals, const std::vector<Action>& actions,
                      const SourceSpan& where) {
    for (const auto& action : actions) {
        if (action.guard) {
            require_expr(context, scope, locals, action.guard.get(), where, "an action guard");
        }
        // ADR-066 item 2: an `If` action carries no target and no value; its `guard` is the
        // condition and its two lists are the branches, validated by the same rules (so a nested
        // `If` is checked the same way). An `If` with no guard has no condition to test.
        if (action.kind == ActionKind::If) {
            if (!action.guard) {
                context.report("TESSERA_IR_MALFORMED", "an `if` action needs a condition", where);
            }
            if (!action.target.empty() || action.value) {
                context.report("TESSERA_IR_MALFORMED",
                               "an `if` action carries no target and no value", where);
            }
            validate_actions(context, scope, locals, action.then_actions, where);
            validate_actions(context, scope, locals, action.else_actions, where);
            continue;
        }
        // ADR-066 item 4: a `dispatch({ type: "x" })` names a reducer state and one of the `type`
        // literals in its dispatch table. The table is the authority, so a literal it does not list
        // is unresolved rather than silently a no-op.
        if (action.kind == ActionKind::Dispatch) {
            if (!starts_with(action.target, "state::") || scope.ids.count(action.target) == 0) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "dispatch target `" + action.target + "` is not a state variable",
                               where);
                continue;
            }
            if (action.action.empty()) {
                context.report("TESSERA_IR_MALFORMED",
                               "a dispatch needs the action's `type` literal", where);
                continue;
            }
            if (action.value) {
                context.report("TESSERA_IR_MALFORMED",
                               "a dispatch in the limited form carries no value", where);
            }
            const std::string& state_id = action.target;
            if (scope.dispatch.count(state_id) == 0) {
                context.report(
                    "TESSERA_IR_UNRESOLVED",
                    "`" + state_id + "` is not a `useReducer` state, so it has no dispatch", where);
            } else if (!has_dispatch_case(scope, state_id, action.action)) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "action `" + action.action + "` is not a case of `" + state_id +
                                   "`'s reducer",
                               where);
            }
            continue;
        }
        if (action.kind == ActionKind::Focus || action.kind == ActionKind::Blur) {
            if (!starts_with(action.target, "ref::") || scope.ids.count(action.target) == 0) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "action target `" + action.target + "` is not a ref", where);
            }
            if (action.value) {
                context.report("TESSERA_IR_MALFORMED",
                               "a `focus` or `blur` action carries no value", where);
            }
            continue;
        }
        if (action.kind == ActionKind::CallProp) {
            const bool is_prop = starts_with(action.target, "prop::") &&
                                 scope.props.count(action.target.substr(6)) != 0;
            if (!is_prop) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "action target `" + action.target + "` is not a callback prop",
                               where);
            } else if (action.value) {
                require_expr(context, scope, locals, action.value.get(), where,
                             "a callback argument");
            }
            continue;
        }
        const char* prefix = action.kind == ActionKind::Set ? "state::" : "anim::";
        if (!starts_with(action.target, prefix) || scope.ids.count(action.target) == 0) {
            context.report(
                "TESSERA_IR_UNRESOLVED",
                "action target `" + action.target + "` is not a " +
                    (action.kind == ActionKind::Set ? "state variable" : "animated value"),
                where);
        }
        // ADR-066 item 4: a `set` whose target is a record-typed state takes a record value, which
        // is the fourth position section 4.3 allows. A reducer case lowers to exactly that â€” one
        // `set` of the state cell to the record the case returned â€” so without this the generated
        // handler for a case could not be written.
        const auto declared = scope.declared_types.find(action.target);
        const bool record_target =
            action.kind == ActionKind::Set && declared != scope.declared_types.end() &&
            record_detail::find_declared_type(
                context, record_detail::innermost_type(declared->second)) != nullptr;
        require_expr(context, scope, locals, action.value.get(), where, "an action value",
                     record_target);
        validate_animation_options(context, action, where);
        validate_on_complete(context, scope, action, where);
        // ADR-067 item 4: only a state setter can be a transition. A transition mark on an
        // animated set would promise a pending UI for a change that never fetches.
        if (action.transition && action.kind != ActionKind::Set) {
            context.report("TESSERA_IR_TRANSITION",
                           "a transition mark is only valid on a state setter", where);
        }
    }
}

} // namespace tessera::compiler::ir
