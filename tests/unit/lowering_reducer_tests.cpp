// SPDX-License-Identifier: MIT

// `useReducer` lowering (ADR-066 item 4): one state cell whose dispatch table maps each action
// `type` literal to the handler its case became, and `dispatch({ type: "x" })` as a `Dispatch`
// action naming the cell and the literal.
//
// This uses the module-level API rather than the single-component one because a reducer state is a
// *record*, and the record type table is what gives it an IR type and a generated struct; the
// single-component entry point has no table, so it can only report the type as unknown.

#include "lowering_module_test_util.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace lowering_module_test;

// The corpus fixture's module, with the component body and cases a test states. `returned` is what
// the component returns; the default is a bare element.
std::string
module_with(std::string_view cases, std::string_view body, std::string_view returned = "<View/>",
            std::string_view types =
                "type CounterState = { count: int };\n"
                "type CounterAction = { type: \"increment\" } | { type: \"reset\" };\n") {
    return std::string(types) +
           "function reduce(state: CounterState, action: CounterAction): CounterState {\n"
           "  switch (action.type) {\n" +
           std::string(cases) +
           "  }\n"
           "}\n"
           "function A() {\n" +
           std::string(body) + "  return " + std::string(returned) + ";\n}\n";
}

constexpr std::string_view kCases = "    case \"increment\":\n"
                                    "      return { count: state.count + 1 };\n"
                                    "    case \"reset\":\n"
                                    "      return { count: 0 };\n";

// A lowered one-file project, or nothing with the first error in `out_error`.
std::optional<lowering::LoweredModule> lower_module(const std::string& source,
                                                    std::string& out_error) {
    Project project;
    project.add(parse_file_raw("ui/Counter.tsx", source));
    lowering::ModuleLoweringResult lowered = project.lower("counter");
    if (!lowered) {
        out_error = lowered.error().empty() ? std::string()
                                            : std::string(lowered.error().front().code) + " | " +
                                                  std::string(lowered.error().front().message);
        return std::nullopt;
    }
    return std::move(*lowered);
}

// The `A` component of a lowered module, or null.
const ir::Component* component_of(const lowering::LoweredModule& module) {
    return find_component(module, "A");
}

void test_state_cell_and_handlers() {
    std::string error;
    const std::optional<lowering::LoweredModule> lowered = lower_module(
        module_with(kCases, "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"),
        error);
    check(lowered.has_value(), "the reducer lowers: " + error);
    if (!lowered) {
        return;
    }
    const ir::Component* component = component_of(*lowered);
    check(component != nullptr, "the component is lowered");
    if (component == nullptr) {
        return;
    }
    check(component->state.size() == 1, "one state cell");
    if (component->state.size() == 1) {
        const ir::State& cell = component->state.front();
        check_eq(cell.id, "state::state", "the cell's id");
        check_eq(cell.type, "CounterState", "the cell's type is the state record");
        check(cell.initial != nullptr && cell.initial->kind == ir::ExprKind::Record,
              "the initial value is a record");
    }
    check(component->handlers.size() == 2, "one handler per case");
}

void test_dispatch_table_keys() {
    std::string error;
    const std::optional<lowering::LoweredModule> lowered = lower_module(
        module_with(kCases, "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"),
        error);
    check(lowered.has_value(), "the reducer lowers: " + error);
    if (!lowered) {
        return;
    }
    const ir::Component* component = component_of(*lowered);
    if (component == nullptr || component->state.size() != 1) {
        check(false, "one state cell to read a dispatch table from");
        return;
    }
    const std::vector<ir::DispatchCase>& table = component->state.front().dispatch;
    check(table.size() == 2, "one dispatch entry per case");
    if (table.size() == 2) {
        check_eq(table[0].action, "increment", "the first case's key");
        check_eq(table[1].action, "reset", "the second case's key");
        check(table[0].handler != table[1].handler, "each case gets its own handler");
    }
}

// A case body reads the cell it writes, so `{ count: state.count + 1 }` is a record whose one
// computed field names `state::state.count` — the dotted id an ordinary record read uses.
void test_case_body_reads_the_cell() {
    std::string error;
    const std::optional<lowering::LoweredModule> lowered = lower_module(
        module_with(kCases, "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"),
        error);
    check(lowered.has_value(), "the reducer lowers: " + error);
    if (!lowered) {
        return;
    }
    const ir::Component* component = component_of(*lowered);
    if (component == nullptr || component->handlers.empty()) {
        check(false, "a handler to read a case body from");
        return;
    }
    const ir::Handler& first = component->handlers.front();
    check(first.actions.size() == 1, "the first case is one action");
    if (first.actions.size() != 1) {
        return;
    }
    const ir::Action& action = first.actions.front();
    check(action.kind == ir::ActionKind::Set, "the action sets the state cell");
    check_eq(action.target, "state::state", "the action targets the cell");
    check(action.value != nullptr && action.value->kind == ir::ExprKind::Record,
          "the value is a record");
    if (action.value == nullptr || action.value->kind != ir::ExprKind::Record ||
        action.value->fields.empty() || !action.value->fields.front().value) {
        return;
    }
    const ir::Expr& field = *action.value->fields.front().value;
    check(field.kind == ir::ExprKind::Binary, "the field is a computed value");
    if (field.kind != ir::ExprKind::Binary || field.operands.size() != 2 || !field.operands[0] ||
        field.operands[0]->kind != ir::ExprKind::Ref) {
        check(false, "the computed value's left operand is a ref");
        return;
    }
    check_eq(field.operands[0]->id, "state::state.count", "the ref names the cell's field");
}

void test_dispatch_call_lowers_to_the_action() {
    std::string error;
    const std::optional<lowering::LoweredModule> lowered = lower_module(
        module_with(kCases, "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n",
                    "(<View onClick={() => dispatch({ type: \"increment\" })}/>)"),
        error);
    check(lowered.has_value(), "a dispatch handler lowers: " + error);
    if (!lowered) {
        return;
    }
    const ir::Component* component = component_of(*lowered);
    if (component == nullptr) {
        check(false, "the component is lowered");
        return;
    }
    check(component->handlers.size() == 3, "the arrow becomes a third handler");
    if (component->handlers.size() != 3) {
        return;
    }
    const ir::Handler& arrow = component->handlers.back();
    check(arrow.actions.size() == 1, "the arrow has one action");
    if (arrow.actions.size() != 1) {
        return;
    }
    const ir::Action& action = arrow.actions.front();
    check(action.kind == ir::ActionKind::Dispatch, "the action is a dispatch");
    check_eq(action.target, "state::state", "the dispatch names the cell");
    check_eq(action.action, "increment", "the dispatch names the type literal");
}

void test_a_payload_dispatch_is_refused() {
    // The dispatch table is keyed by the `type` literal, so a payload has nowhere to go.
    std::string error;
    const std::optional<lowering::LoweredModule> lowered = lower_module(
        module_with(kCases, "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n",
                    "(<View onClick={() => dispatch({ type: \"increment\", by: 2 })}/>)"),
        error);
    check(!lowered.has_value(), "a payload dispatch produces no IR");
    check(error.find("TESSERA_REACT_REDUCER_FORM") != std::string::npos,
          "the code is the form error (saw `" + error + "`)");
}

} // namespace

int main() {
    test_state_cell_and_handlers();
    test_dispatch_table_keys();
    test_case_body_reads_the_cell();
    test_dispatch_call_lowers_to_the_action();
    test_a_payload_dispatch_is_refused();
    return finish("lowering reducer tests");
}
