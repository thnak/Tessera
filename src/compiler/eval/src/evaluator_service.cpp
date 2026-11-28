// SPDX-License-Identifier: MIT

// The actions that leave the value model (docs/specs/ir-v1.md section 2.1): focus, `callProp` and
// `callService`. Split out of evaluator.cpp, which is at the 500-line hard limit; the subject is
// the effect a host models, not the state/animation assignment the rest of `run` performs.

#include "tessera/compiler/eval/evaluator.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::eval {

void Evaluator::start_transition(const std::string& target, const Scope& scope) {
    if (const ir::Component* component = pending_->component(); component != nullptr) {
        for (const ir::Behavior& behavior : component->behaviors) {
            if (behavior.behavior != "deferred_value" || behavior.args.empty()) {
                continue;
            }
            if (auto current = eval(behavior.args.front().expr, scope, 0)) {
                (void)pending_->deferred(behavior.id, *current);
            }
        }
    }
    pending_->begin_transition(target);
}

std::expected<Value, EvalError> Evaluator::unresolved_ref(const ir::Expr& expr,
                                                          const Scope& scope) {
    // pending-ui-v1.md sections 5 and 6: `useTransition` and `useDeferredValue` are behaviors whose
    // output the pending session owns, so a bound session answers their `beh::` reads.
    if (pending_ != nullptr && pending_->component() != nullptr && expr.id.rfind("beh::", 0) == 0) {
        for (const ir::Behavior& behavior : pending_->component()->behaviors) {
            if (behavior.outputs.empty() ||
                behavior.id + "." + behavior.outputs.front().name != expr.id) {
                continue;
            }
            if (behavior.behavior == "transition") {
                return Value::of_bool(pending_->is_pending());
            }
            if (behavior.behavior == "deferred_value" && !behavior.args.empty()) {
                auto current = eval(behavior.args.front().expr, scope, 0);
                if (!current) {
                    return std::unexpected(std::move(current.error()));
                }
                return pending_->deferred(behavior.id, *current);
            }
        }
    }
    // #656: a resource's status comes from the pending session when the host bound one.
    const std::size_t dot = expr.id.rfind('.');
    if (pending_ != nullptr && dot != std::string::npos) {
        const std::string_view base = std::string_view(expr.id).substr(0, dot);
        const std::string_view member = std::string_view(expr.id).substr(dot + 1);
        if (member == "status") {
            if (const auto status = pending_->status_of(base)) {
                return Value::of_string(std::string(*status));
            }
        } else if (member == "data") {
            if (auto data = pending_->data_of(base)) {
                return std::move(*data);
            }
        } else if (member == "error") {
            if (const auto text = pending_->error_of(base)) {
                return Value::of_string(std::string(*text));
            }
        }
    }
    return std::unexpected(
        EvalError{"TESSERA_IR_EVAL_UNKNOWN_REF", "`" + expr.id + "` is not in scope"});
}

std::expected<void, EvalError> Evaluator::run_host_action(const ir::Action& action,
                                                          const Scope& scope) {
    if (action.kind == ir::ActionKind::CallProp) {
        notify_prop_call(action); // the caller's callback, if a host models the caller
        return {};
    }
    if (action.kind != ir::ActionKind::CallService) {
        return {}; // focus is host state, not a state/anim value: the evaluator has no scene
    }
    // ADR-050 section 3.1 item 4: the arguments are evaluated in call order even without a sink, so
    // a malformed argument is the same error whether or not a host models the service.
    std::vector<Value> args;
    args.reserve(action.args.size());
    for (const ir::ExprPtr& arg : action.args) {
        if (!arg) {
            return std::unexpected(
                EvalError{"TESSERA_IR_EVAL_MALFORMED", "a service call argument is missing"});
        }
        auto value = eval(*arg, scope, 0);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        args.push_back(std::move(*value));
    }
    if (service_sink_ != nullptr) {
        service_sink_(service_sink_context_, action.target, action.method, args);
    }
    return {};
}

} // namespace tessera::compiler::eval
