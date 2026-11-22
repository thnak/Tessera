// SPDX-License-Identifier: MIT

// `useReducer` in its limited form (ADR-066 item 4): the accepted shape, the symbol table it
// declares, and each way out of the form. The form is defined by a module-scope helper and the
// type aliases beside it, so every fixture needs a whole module rather than one component body.

#include "rules_test_util.hpp"

namespace {

using namespace rules_test;

// A module whose reducer is written by `reducer` and whose component body is `body`. The state and
// action types are the corpus fixture's, so a test states only the part it is about.
std::string module_with(std::string_view reducer, std::string_view body,
                        std::string_view types =
                            "type CounterState = { count: int };\n"
                            "type CounterAction = { type: \"increment\" } | { type: \"reset\" };\n",
                        std::string_view component_params = "") {
    return std::string(types) + std::string(reducer) + "\nfunction A(" +
           std::string(component_params) + ") {\n" + std::string(body) + "  return <View/>;\n}\n";
}

// The corpus fixture's reducer, optionally with `body` in place of its cases.
std::string reduce_with(std::string_view cases,
                        std::string_view signature = "function reduce(state: CounterState, "
                                                     "action: CounterAction): CounterState") {
    return std::string(signature) + " {\n  switch (action.type) {\n" + std::string(cases) +
           "  }\n}\n";
}

constexpr std::string_view kCases = "    case \"increment\":\n"
                                    "      return { count: state.count + 1 };\n"
                                    "    case \"reset\":\n"
                                    "      return { count: 0 };\n";

const rules::ComponentAnalysis& only_component(const Fixture& fixture) {
    static const rules::ComponentAnalysis kEmpty;
    check(fixture.analyzed.components.size() == 1, "one component analysed");
    return fixture.analyzed.components.empty() ? kEmpty : fixture.analyzed.components[0];
}

void test_accepted_form() {
    const auto fixture = analyze(module_with(
        reduce_with(kCases), "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check_eq(describe_all(*fixture), "", "the limited form is accepted");
    const auto& analysis = only_component(*fixture);
    check(analysis.ok, "the component is ok");
    check_eq(symbol_text(analysis), "state:State dispatch:Dispatch ",
             "the reducer declares a state and a dispatch, not a setter");
    check(analysis.reducers.size() == 1, "one reducer recorded");
    if (analysis.reducers.size() != 1) {
        return;
    }
    const rules::ReducerInfo& info = analysis.reducers[0];
    check_eq(info.name, "state", "the state binding");
    check_eq(info.dispatch, "dispatch", "the dispatch binding");
    check_eq(info.reducer, "reduce", "the helper");
    check_eq(info.state_type, "CounterState", "the state record name");
    check(info.cases.size() == 2, "one case per action type");
    check(!info.has_default, "no default clause");
    if (info.cases.size() == 2) {
        check_eq(info.cases[0].action, "increment", "the first case's action");
        check_eq(info.cases[1].action, "reset", "the second case's action");
    }
}

void test_default_clause_is_recorded() {
    // A `default` adds no dispatch key: the generated handler set is still one per action type, so
    // the clause is accepted and recorded rather than being a second way to say the same thing.
    const auto fixture = analyze(
        module_with(reduce_with(std::string(kCases) + "    default:\n      return { count: 0 };\n"),
                    "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check_eq(describe_all(*fixture), "", "a `default` clause is accepted");
    const auto& analysis = only_component(*fixture);
    check(analysis.reducers.size() == 1 && analysis.reducers[0].has_default,
          "the default clause is recorded");
    check(analysis.reducers.size() == 1 && analysis.reducers[0].cases.size() == 2,
          "a default clause adds no case");
}

void test_dispatch_only_body_is_an_inline_handler() {
    // A handler whose body is dispatch calls needs no native binding, exactly as one whose body is
    // setter calls does.
    const auto fixture = analyze(module_with(
        reduce_with(kCases), "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"
                             "  const add = () => {\n"
                             "    dispatch({ type: \"increment\" });\n"
                             "  };\n"));
    check_eq(describe_all(*fixture), "", "a dispatch-only handler body is accepted");
    const auto& analysis = only_component(*fixture);
    check_eq(role_text(analysis), "InlineHandler ", "the handler is an inline handler");
    check(analysis.find("dispatch") != nullptr, "the dispatch is a symbol");
}

void test_reducer_must_be_a_module_helper() {
    // An arrow at module scope, or a name the module does not declare, has no body the lowering
    // can read.
    const auto fixture = analyze(
        module_with("", "  const [state, dispatch] = useReducer(undefinedName, { count: 0 });\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_REDUCER_FORM | 5:40 | undefinedName | `undefinedName` is not a "
             "module-scope `function` declared in this file; the limited form needs its body to "
             "lower\n",
             "an undeclared reducer is a form error");
}

void test_reducer_must_switch_on_the_action_type() {
    const auto fixture = analyze(
        module_with("function reduce(state: CounterState, action: CounterAction): CounterState {\n"
                    "  switch (state.count) {\n"
                    "    case \"increment\":\n"
                    "      return { count: 1 };\n"
                    "  }\n}\n",
                    "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check(describe_all(*fixture).find("must switch on `action.type`") != std::string::npos,
          "switching on something other than the action type is a form error");
}

void test_every_action_needs_a_case() {
    // A missing case would leave an action the union declares with no handler, so
    // `dispatch({ type: "reset" })` would compile and do nothing.
    const auto fixture = analyze(
        module_with("function reduce(state: CounterState, action: CounterAction): CounterState {\n"
                    "  switch (action.type) {\n"
                    "    case \"increment\":\n"
                    "      return { count: state.count + 1 };\n"
                    "  }\n}\n",
                    "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_REDUCER_FORM | 3:10 | reduce | reducer `reduce` does not handle the "
             "action `reset` that `CounterAction` declares\n",
             "a declared action with no case is a form error");
}

void test_case_outside_the_union_is_rejected() {
    const auto fixture = analyze(module_with(
        reduce_with(std::string(kCases) + "    case \"decrement\":\n      return { count: 0 };\n"),
        "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check(describe_all(*fixture).find("`decrement` is not a `type` of `CounterAction`") !=
              std::string::npos,
          "a case the union does not declare is a form error");
}

void test_case_must_return_the_state_record() {
    const auto fixture =
        analyze(module_with(reduce_with(std::string_view("    case \"increment\":\n"
                                                         "      return state.count + 1;\n"
                                                         "    case \"reset\":\n"
                                                         "      return { count: 0 };\n")),
                            "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check(describe_all(*fixture).find("must return a record of `CounterState`") !=
              std::string::npos,
          "a case that returns a non-record is a form error");
}

void test_two_statements_in_a_case_are_rejected() {
    // A reducer case is exactly one `return` of a record: a local or a second statement is outside
    // the form, because the case becomes one generated handler with one assignment.
    const auto fixture =
        analyze(module_with(reduce_with(std::string_view("    case \"increment\":\n"
                                                         "      const next = state.count + 1;\n"
                                                         "      return { count: next };\n"
                                                         "    case \"reset\":\n"
                                                         "      return { count: 0 };\n")),
                            "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check(describe_all(*fixture).find("must be one `return <record>;`") != std::string::npos,
          "a case with a local is a form error");
}

void test_payload_action_is_rejected() {
    // The dispatch table is keyed by the `type` literal, so an action that carries a value has no
    // place in the limited form.
    const auto fixture = analyze(module_with(
        reduce_with(kCases), "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n",
        "type CounterState = { count: int };\n"
        "type CounterAction = { type: \"increment\"; by: int } | { type: \"reset\" };\n"));
    check(describe_all(*fixture).find("a dispatch in the limited form carries no value") !=
              std::string::npos,
          "a payload-carrying action is a form error");
}

void test_reducer_must_return_its_state_type() {
    const auto fixture = analyze(module_with(
        reduce_with(kCases, "function reduce(state: CounterState, action: CounterAction): int"),
        "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    check(describe_all(*fixture).find("must return its state type `CounterState`") !=
              std::string::npos,
          "a reducer that does not return its state is a form error");
}

void test_statement_shape_is_checked() {
    // One binding, not two; and no bare statement.
    const auto single = analyze(
        module_with(reduce_with(kCases), "  const state = useReducer(reduce, { count: 0 });\n"));
    check(single->analyzed.diagnostics.size() == 1, "a single binding is a form error");
    check_eq(std::string(single->analyzed.diagnostics.empty()
                             ? std::string_view()
                             : single->analyzed.diagnostics.front().code),
             "TESSERA_REACT_REDUCER_FORM", "the code");
    const auto bare =
        analyze(module_with(reduce_with(kCases), "  useReducer(reduce, { count: 0 });\n"));
    check(bare->analyzed.diagnostics.size() == 1, "a bare statement is a form error");
}

void test_use_reducer_is_a_known_hook() {
    // It must not report UNSUPPORTED_API or UNKNOWN_HOOK any more: it is a core hook whose form is
    // checked by its own pass.
    const auto fixture = analyze(module_with(
        reduce_with(kCases), "  const [state, dispatch] = useReducer(reduce, { count: 0 });\n"));
    const std::string text = describe_all(*fixture);
    check(text.find("TESSERA_REACT_UNSUPPORTED_API") == std::string::npos,
          "`useReducer` is not an unsupported API");
    check(text.find("TESSERA_REACT_UNKNOWN_HOOK") == std::string::npos,
          "`useReducer` is not an unknown hook");
}

void test_nested_use_reducer_is_a_placement_error() {
    const auto fixture = analyze(module_with(
        reduce_with(kCases), "  const ternary = flag ? useReducer(reduce, { count: 0 }) : 0;\n"));
    check(describe_all(*fixture).find("hook `useReducer` is called inside a condition") !=
              std::string::npos,
          "a nested useReducer is a placement error, not a form error");
}

} // namespace

int main() {
    test_accepted_form();
    test_default_clause_is_recorded();
    test_dispatch_only_body_is_an_inline_handler();
    test_reducer_must_be_a_module_helper();
    test_reducer_must_switch_on_the_action_type();
    test_every_action_needs_a_case();
    test_case_outside_the_union_is_rejected();
    test_case_must_return_the_state_record();
    test_two_statements_in_a_case_are_rejected();
    test_payload_action_is_rejected();
    test_reducer_must_return_its_state_type();
    test_statement_shape_is_checked();
    test_use_reducer_is_a_known_hook();
    test_nested_use_reducer_is_a_placement_error();
    return finish("rules reducer tests");
}
