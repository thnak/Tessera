// SPDX-License-Identifier: MIT

// The actions that leave the value model (docs/specs/ir-v1.md section 2.1): focus, `callProp` and
// `callService`. Split out of evaluator.cpp, which is at the 500-line hard limit; the subject is
// the effect a host models, not the state/animation assignment the rest of `run` performs.

#include "tessera/compiler/eval/evaluator.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::eval {

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
