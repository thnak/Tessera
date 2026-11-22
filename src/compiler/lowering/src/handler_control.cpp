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
        std::optional<ir::Action> action = lower_setter_call(state, statement, callee, names);
        if (!action && !state.error) {
            action = lower_callback_call(state, statement, callee, arrow);
        }
        return action;
    }
    return std::nullopt;
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