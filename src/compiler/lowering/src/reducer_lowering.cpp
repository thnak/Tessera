// SPDX-License-Identifier: MIT

// `useReducer` lowering (ADR-066 item 4). The recognition pass (`rules/reducer.cpp`) has already
// decided the source is in the accepted form and resolved the whole shape — the state record, the
// action union and every case's returned record — so this only emits it:
//
//   const [state, dispatch] = useReducer(reduce, { count: 0 })
//
// becomes one state cell of the state record type (an ordinary record-valued `useState` in the IR)
// whose `dispatch` table maps each action `type` literal to the handler its case became, plus one
// handler per case. `dispatch({ type: "x" })` then lowers to a `Dispatch` action naming the cell
// and the literal, and the generated listener indexes the table.
//
// A case body is `return { count: state.count + 1 }`. Its field values are read from the cell the
// handler is about to assign, so the recorded expression is a record whose computed field is
// `state::state.count` — the same dotted id an ordinary record read uses. The reducer's `state`
// parameter is therefore not a local: it *is* the cell, which is what makes the reducer a pure
// function of `(state, action)` with no second storage.

#include "hook_lowering_internal.hpp"
#include "record_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

std::string prefixed(std::string_view prefix, const std::string& name) {
    return std::string(prefix) + name;
}

const rules::ReducerInfo* reducer_at(const HookState& state, std::size_t index) {
    for (const rules::ReducerInfo& candidate : state.context.analysis.reducers) {
        if (candidate.statement == index) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

void lower_reducer_hook(HookState& state, std::size_t index, LoweredHooks& out,
                        const NameResolver& names) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::ReducerInfo* info = reducer_at(state, index);
    if (info == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "the analysis did not classify this `useReducer`",
                  statement.span);
        return;
    }
    // The state record gives the cell its IR type and the generated aggregate its struct. Without
    // the table a reducer state has no C++ form, so it is an error rather than a scalar cell.
    const ir::RecordType* record = nullptr;
    if (context.records != nullptr) {
        record = find_record(*context.records, info->state_type);
    }
    if (record == nullptr) {
        fail_hook(state, "TESSERA_IR_UNKNOWN_TYPE",
                  "the state type `" + info->state_type + "` of reducer `" + info->reducer +
                      "` is not in the record type table",
                  statement.span);
        return;
    }
    if (statement.init->operands.size() != 3 || !statement.init->operands[2]) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "`useReducer` has no initial value",
                  statement.span);
        return;
    }
    const jsx::Expr& initial = *statement.init->operands[2];
    if (initial.kind != jsx::ExprKind::Object) {
        fail_hook(state, "TESSERA_IR_RECORD_VALUE",
                  "the initial value of reducer `" + info->reducer + "` must be a `" +
                      info->state_type + "` record literal",
                  initial.span);
        return;
    }
    // The initial value is a record of its own fields, so it lowers with the component's names (it
    // may read a prop) and no case parameter.
    RecordLoweringState initial_state{context.source_map, context.file, std::nullopt, &names};
    ir::ExprPtr initial_value =
        lower_record_value(initial_state, initial, *record, *context.records);
    if (!initial_value) {
        if (initial_state.error && !state.error) {
            state.error = std::move(initial_state.error);
        }
        return;
    }
    const std::string state_id = prefixed("state::", info->name);
    ir::State cell;
    cell.id = state_id;
    cell.name = info->name;
    cell.type = info->state_type;
    cell.initial = std::move(initial_value);
    // One handler per case, in `case` order, each with no parameters: the dispatch that reaches it
    // already selected the case, so there is no payload to read. Its body is one `set` of the state
    // cell to the record the case returned.
    for (const rules::ReducerCase& reducer_case : info->cases) {
        if (reducer_case.body == nullptr || reducer_case.body->kind != jsx::ExprKind::Object) {
            fail_hook(state, "TESSERA_IR_RECORD_VALUE",
                      "case `" + reducer_case.action + "` of reducer `" + info->reducer +
                          "` must return a `" + info->state_type + "` record literal",
                      reducer_case.span);
            return;
        }
        // A case body reads its own state through the same resolver: the analysis declared the
        // reducer's state binding as a `State` symbol, so `state.count` already resolves to
        // `state::state.count` — the field the handler is about to write. Binding a local of the
        // same name would shadow that and produce a bare `state.count`.
        ComponentNames case_names(context.analysis);
        RecordLoweringState case_state{context.source_map, context.file, std::nullopt, &case_names};
        ir::ExprPtr value =
            lower_record_value(case_state, *reducer_case.body, *record, *context.records);
        if (!value) {
            if (case_state.error && !state.error) {
                state.error = std::move(case_state.error);
            }
            return;
        }
        ir::Action action;
        action.kind = ir::ActionKind::Set;
        action.target = state_id;
        action.value = std::move(value);
        std::vector<ir::Action> actions;
        actions.push_back(std::move(action));
        const std::string handler_id =
            out.add_handler({}, std::move(actions),
                            to_source_span(context.source_map, reducer_case.span, context.file));
        cell.dispatch.push_back(ir::DispatchCase{reducer_case.action, handler_id});
    }
    out.state.push_back(std::move(cell));
    // `dispatch` is a function value, so it resolves to nothing and a call of it lowers to a
    // `Dispatch` action; only the state cell joins the name table.
    (void)names;
}

} // namespace tessera::compiler::lowering::internal
