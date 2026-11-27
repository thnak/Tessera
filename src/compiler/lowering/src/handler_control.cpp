// SPDX-License-Identifier: MIT

// Control flow in a handler body (ADR-066 item 2): `if`/`else` becomes one `If` action, `const` a
// local that is inlined, and an early `return;` ends the list it appears in. Split out of
// `handler_actions.cpp` when that file reached the 500-line hard limit; the subject is the
// statement forms, not the calls they contain.

#include "handler_control_internal.hpp"

namespace tessera::compiler::lowering::internal {

std::optional<ir::Action> lower_call_action(HookState& state, const jsx::Expr& statement,
                                            const jsx::Expr& arrow, ComponentNames& names) {
    if (statement.kind != jsx::ExprKind::Call || statement.operands.empty() ||
        !statement.operands[0]) {
        return std::nullopt;
    }
    const jsx::Expr& callee = *statement.operands[0];
    // A service method is matched first: its name may be `focus` or `set`, and the receiver's
    // symbol kind, not the method name, says which form this is.
    if (callee.kind == jsx::ExprKind::Member && callee.operands.size() == 1 && callee.operands[0] &&
        callee.operands[0]->kind == jsx::ExprKind::Identifier) {
        std::optional<ir::Action> service = lower_service_call(state, statement, callee, names);
        if (service || state.error) {
            return service;
        }
    }
    if (callee.kind == jsx::ExprKind::Member && (callee.text == "focus" || callee.text == "blur") &&
        callee.operands.size() == 1 && callee.operands[0] &&
        callee.operands[0]->kind == jsx::ExprKind::Identifier) {
        return lower_ref_focus(state, statement, callee);
    }
    if (callee.kind == jsx::ExprKind::Member && callee.text == "set" &&
        callee.operands.size() == 1 && callee.operands[0] &&
        callee.operands[0]->kind == jsx::ExprKind::Identifier) {
        return lower_animated_set(state, statement, callee, names);
    }
    if (callee.kind == jsx::ExprKind::Identifier) {
        // ADR-067 item 4: `startTransition(() => setX(v))` is the setter call with a transition
        // mark, so it is matched before the plain setter path.
        std::optional<ir::Action> transition =
            lower_transition_call(state, statement, callee, names);
        if (transition || state.error) {
            return transition;
        }
        // ADR-066 item 4: a dispatch is a state update, not a value, so it is matched before the
        // setter path (which looks up a setter symbol and would not find one).
        std::optional<ir::Action> dispatch = lower_dispatch_call(state, statement, callee);
        if (dispatch) {
            return dispatch;
        }
        std::optional<ir::Action> action = lower_setter_call(state, statement, callee, names);
        if (!action && !state.error) {
            action = lower_callback_call(state, statement, callee, arrow);
        }
        return action;
    }
    return std::nullopt;
}

// `startTransition(() => setX(v))` (ADR-067 item 4, docs/specs/pending-ui-v1.md section 5). It
// lowers to the action the inner setter call would lower to, plus `transition: true`. The argument
// is one parameterless arrow with one state-setter call; the analyzer checked the shape, and the
// check is repeated here so a hand-built analysis cannot reach the validator with an unmarkable
// action.
std::optional<ir::Action> lower_transition_call(HookState& state, const jsx::Expr& call,
                                                const jsx::Expr& callee, ComponentNames& names) {
    const rules::Symbol* start = state.context.analysis.find(callee.text);
    if (start == nullptr || start->kind != rules::SymbolKind::TransitionStart) {
        return std::nullopt; // not a startTransition: the caller tries the other call forms
    }
    constexpr std::string_view kShape = "a transition is `startTransition(() => setX(value))`: one "
                                        "arrow with one state setter call";
    const jsx::Expr* arrow = call.operands.size() == 2 ? call.operands[1].get() : nullptr;
    if (arrow == nullptr || arrow->kind != jsx::ExprKind::Arrow || !arrow->params.empty() ||
        arrow->operands.size() != 1 || !arrow->operands[0]) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED", kShape, call.span);
        return std::nullopt;
    }
    const jsx::Expr& inner = *arrow->operands[0];
    if (inner.kind != jsx::ExprKind::Call || inner.operands.empty() || !inner.operands[0] ||
        inner.operands[0]->kind != jsx::ExprKind::Identifier) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED", kShape, inner.span);
        return std::nullopt;
    }
    std::optional<ir::Action> action = lower_setter_call(state, inner, *inner.operands[0], names);
    if (!action) {
        if (!state.error) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", kShape, inner.span);
        }
        return std::nullopt;
    }
    action->transition = true;
    return action;
}

// `dispatch({ type: "x" })` (ADR-066 item 4). The generated listener indexes the state's dispatch
// table by the `type` literal and calls the handler that case became, so the IR action carries the
// cell's id and the literal — no handler id, because which one runs is the table's answer.
//
// The argument has to be an object literal with a string-literal `type`: the table is keyed by a
// literal, so a computed type would need a run-time lookup. A second member is the payload the
// limited form does not have.
std::optional<ir::Action> lower_dispatch_call(HookState& state, const jsx::Expr& call,
                                              const jsx::Expr& callee) {
    const rules::Symbol* dispatch = state.context.analysis.find(callee.text);
    if (dispatch == nullptr || dispatch->kind != rules::SymbolKind::Dispatch) {
        return std::nullopt; // not a dispatch: the caller tries the other call forms
    }
    constexpr std::string_view kShape =
        "a dispatch is `dispatch({ type: \"x\" })`, where `x` is a `type` of the reducer's action "
        "union";
    if (call.operands.size() != 2 || !call.operands[1]) {
        fail_hook(state, "TESSERA_REACT_REDUCER_FORM", kShape, call.span);
        return std::nullopt;
    }
    const jsx::Expr& argument = *call.operands[1];
    if (argument.kind != jsx::ExprKind::Object) {
        fail_hook(state, "TESSERA_REACT_REDUCER_FORM", kShape, argument.span);
        return std::nullopt;
    }
    const jsx::ObjectProperty* type_property = nullptr;
    for (const jsx::ObjectProperty& property : argument.properties) {
        if (property.key == "type") {
            type_property = &property;
            continue;
        }
        // A payload the limited form cannot carry: the dispatch table has no slot for it.
        fail_hook(state, "TESSERA_REACT_REDUCER_FORM",
                  "`" + property.key +
                      "` is not part of a dispatch in the limited form; an action carries its "
                      "`type` only",
                  property.key_span, kShape);
        return std::nullopt;
    }
    if (type_property == nullptr || !type_property->value ||
        type_property->value->kind != jsx::ExprKind::String) {
        fail_hook(state, "TESSERA_REACT_REDUCER_FORM",
                  "a dispatch needs a `type` whose value is a string literal, because the dispatch "
                  "table is keyed by it",
                  argument.span, kShape);
        return std::nullopt;
    }
    // The state a dispatch belongs to is the one declared by the same statement.
    const rules::Symbol* cell = nullptr;
    for (const rules::Symbol& symbol : state.context.analysis.symbols) {
        if (symbol.kind == rules::SymbolKind::State && symbol.statement == dispatch->statement) {
            cell = &symbol;
            break;
        }
    }
    if (cell == nullptr) {
        fail_hook(state, "TESSERA_IR_UNRESOLVED",
                  "`" + callee.text + "` has no reducer state on its statement", call.span);
        return std::nullopt;
    }
    ir::Action action;
    action.kind = ir::ActionKind::Dispatch;
    action.target = "state::" + cell->name;
    action.action = type_property->value->text;
    return action;
}

// `nav.push(route)` (ADR-050 section 3.1 item 4). The analyzer already checked the method and the
// argument count against the manifest, so this turns the call into an action: the requirement's
// `svc::` id, the method name and the lowered arguments in call order. An argument is a pure
// expression like any action value, so it may read props, state and the handler's parameters.
std::optional<ir::Action> lower_service_call(HookState& state, const jsx::Expr& call,
                                             const jsx::Expr& callee, ComponentNames& names) {
    const rules::Symbol* receiver = state.context.analysis.find(callee.operands[0]->text);
    if (receiver == nullptr || receiver->kind != rules::SymbolKind::Service) {
        return std::nullopt; // not a service: the caller tries the other call forms
    }
    ir::Action action;
    action.kind = ir::ActionKind::CallService;
    action.target = "svc::" + receiver->name;
    action.method = callee.text;
    for (std::size_t index = 1; index < call.operands.size(); ++index) {
        if (!call.operands[index]) {
            fail_hook(state, "TESSERA_IR_MALFORMED", "a service call has an empty argument",
                      call.span);
            return std::nullopt;
        }
        ir::ExprPtr argument = lower_component_expression(state, *call.operands[index], names);
        if (!argument) {
            return std::nullopt;
        }
        action.args.push_back(std::move(argument));
    }
    return action;
}

// One `if`/`else` statement as an `If` action (ADR-066 item 2). The condition is a pure bool
// expression; each branch is a `Block` whose statements lower through the same rules as the body
// they appear in, so a nested `if` is an `If` inside a branch list.
std::optional<ir::Action> lower_if_action(HookState& state, const jsx::Expr& statement,
                                          const jsx::Expr& arrow, ComponentNames& names) {
    if (statement.operands.size() < 2 || statement.operands.size() > 3 || !statement.operands[0] ||
        !statement.operands[1]) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "an `if` needs a condition and a branch",
                  statement.span);
        return std::nullopt;
    }
    ir::ExprPtr condition = lower_component_expression(state, *statement.operands[0], names);
    if (state.error || !condition) {
        return std::nullopt;
    }
    ir::Action action;
    action.kind = ir::ActionKind::If;
    action.guard = std::move(condition);
    action.then_actions = lower_branch(state, *statement.operands[1], arrow, names);
    if (state.error) {
        return std::nullopt;
    }
    if (statement.operands.size() == 3 && statement.operands[2]) {
        action.else_actions = lower_branch(state, *statement.operands[2], arrow, names);
        if (state.error) {
            return std::nullopt;
        }
    }
    return action;
}

// The statements of one `if`/`else` branch. A branch is a `Block` (the parser wraps a bare
// statement in one), except an `else if`, which nests an `If` directly; both shapes are handled
// here.
std::vector<ir::Action> lower_branch(HookState& state, const jsx::Expr& branch,
                                     const jsx::Expr& arrow, ComponentNames& names) {
    std::vector<ir::Action> actions;
    if (branch.kind == jsx::ExprKind::If) {
        std::optional<ir::Action> nested = lower_if_action(state, branch, arrow, names);
        if (state.error || !nested) {
            return actions;
        }
        actions.push_back(std::move(*nested));
        return actions;
    }
    if (branch.kind != jsx::ExprKind::Block) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "an `if` branch is a block", branch.span);
        return actions;
    }
    for (const jsx::ExprPtr& statement : branch.operands) {
        if (!statement) {
            fail_hook(state, "TESSERA_IR_MALFORMED", "a branch statement is missing", branch.span);
            return actions;
        }
        if (statement->kind == jsx::ExprKind::If) {
            std::optional<ir::Action> nested = lower_if_action(state, *statement, arrow, names);
            if (state.error || !nested) {
                return actions;
            }
            actions.push_back(std::move(*nested));
            continue;
        }
        if (statement->kind == jsx::ExprKind::Const) {
            if (statement->operands.size() != 1 || !statement->operands[0]) {
                fail_hook(state, "TESSERA_IR_MALFORMED", "a branch local needs one initializer",
                          statement->span);
                return actions;
            }
            names.add_const_local(statement->text, statement->operands[0].get());
            continue;
        }
        if (statement->kind == jsx::ExprKind::Return) {
            if (!statement->operands.empty()) {
                fail_hook(
                    state, "TESSERA_IR_UNSUPPORTED",
                    "`return <value>` is only allowed as the last statement of a preview body",
                    statement->span, "write `return;` to leave the handler early");
                return actions;
            }
            break;
        }
        // A setter, `.set`, `focus`/`blur` or callback call: the same rules as the body.
        std::optional<ir::Action> action = lower_call_action(state, *statement, arrow, names);
        if (state.error || !action) {
            return actions;
        }
        actions.push_back(std::move(*action));
    }
    return actions;
}

// The first `count` statements of `arrow` as actions.
//
// ADR-066 item 2 adds three statement forms to a handler body, and each lowers to the same action
// list rather than to a new execution model:
//

std::optional<ir::Preview> lower_preview(HookState& state, const jsx::Expr& arrow) {
    if (arrow.kind != jsx::ExprKind::Arrow || arrow.operands.empty()) {
        return std::nullopt;
    }
    // A scratch state: a body outside the preview rules is not an error here.
    HookState scratch{state.context, std::nullopt};
    std::size_t count = arrow.operands.size();
    ir::ExprPtr return_value;
    const jsx::ExprPtr& last = arrow.operands.back();
    if (last && last->kind == jsx::ExprKind::Return) {
        --count;
        if (last->operands.size() == 1 && last->operands[0]) {
            ComponentNames names(scratch.context.analysis);
            for (const std::string& param : arrow.params) {
                names.add_local(param);
            }
            return_value = lower_component_expression(scratch, *last->operands[0], names);
            if (scratch.error || !return_value) {
                return std::nullopt;
            }
        }
    }
    std::vector<ir::Action> actions = lower_statements(scratch, arrow, count);
    if (scratch.error) {
        return std::nullopt;
    }
    return ir::Preview{std::move(actions), std::move(return_value)};
}

std::vector<ir::Param> lower_handler_params(HookState& state, const jsx::Expr& arrow) {
    std::vector<ir::Param> params;
    if (arrow.params.size() != arrow.param_types.size()) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "a handler arrow has one parameter type per parameter", arrow.span);
        return params;
    }
    for (std::size_t i = 0; i < arrow.params.size(); ++i) {
        const std::string& name = arrow.params[i];
        if (name.empty()) {
            fail_hook(state, "TESSERA_IR_MALFORMED", "a handler parameter has no name", arrow.span);
            return params;
        }
        if (!arrow.param_types[i]) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                      "parameter `" + name +
                          "` of a declared handler needs a type annotation; the event parameter "
                          "types of the element table are slice 4",
                      arrow.span, "annotate the parameter type");
            return params;
        }
        const std::optional<std::string> type = lower_written_type(*arrow.param_types[i]);
        if (!type) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                      "parameter `" + name + "` has a type with no IR form in v1",
                      arrow.param_types[i]->span);
            return params;
        }
        params.push_back(ir::Param{name, *type});
    }
    return params;
}
} // namespace tessera::compiler::lowering::internal