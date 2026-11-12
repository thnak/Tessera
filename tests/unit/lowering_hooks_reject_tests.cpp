// SPDX-License-Identifier: MIT

// Slice 6 rejection cases: diagnostics are TESSERA_IR_*, lowering stops at the first error and a
// rejected component produces no IR at all (docs/design/jsx-ir-lowering.md sections 4 and 5).

#include "lowering_hooks_test_util.hpp"

#include <string>

namespace {

using namespace lowering_hooks_test;

void test_effect_needs_a_binding() {
    const auto fixture = parse(component("  useEffect(() => { go(); }, []);\n"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "an unannotated effect has no IR form");
    check_eq(describe_errors(*fixture, result),
             "TESSERA_IR_BINDING | 2:3 | useEffect(() => { go(); }, []); | a `useEffect` needs a "
             "`// Note: bind::<id>` annotation: its body is a protected region in generated C++\n",
             "the missing annotation is reported at the statement");
}

void test_state_type_needs_the_type_table() {
    const auto unsupported = parse(component("  const [x, setX] = useState<{ a: int }>(null);\n"));
    const auto result = lower(*unsupported);
    check(!result.has_value(), "an inline object state type has no IR form");
    if (!result.has_value() && !result.error().empty()) {
        check_eq(std::string(result.error()[0].code), "TESSERA_IR_UNSUPPORTED", "the code");
        check_eq(std::string(unsupported->map.slice(result.error()[0].span.span)), "{ a: int }",
                 "the type argument span is reported");
    }

    const auto from_prop =
        parse(component("  const [x, setX] = useState(props.a);\n", "<View/>", "props: P"));
    const auto from_prop_result = lower(*from_prop);
    check(!from_prop_result.has_value(),
          "an initial value that needs expression typing asks for a type argument");
    check_eq(first_code(from_prop_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto null_init = parse(component("  const [x, setX] = useState(null);\n"));
    const auto null_result = lower(*null_init);
    check(!null_result.has_value(), "a null initial without <T> has no determinable IR type");
    check_eq(first_code(null_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_array_list_initial_rejections() {
    // `useState<T[]>(...)` accepts an array literal with literal elements and a list reference
    // (spec 8.4); every other list-valued initial has no IR form and is rejected loudly.
    const auto no_type = parse(component("  const [rows, setRows] = useState([]);\n"));
    const auto no_type_result = lower(*no_type);
    check(!no_type_result.has_value(), "an array initial without an element type is rejected");
    check_eq(first_code(no_type_result), "TESSERA_IR_UNSUPPORTED", "the code names the slice");

    const auto dynamic_element =
        parse(component("  const [name, setName] = useState(\"a\");\n"
                        "  const [rows, setRows] = useState<string[]>([name]);\n"));
    const auto dynamic_result = lower(*dynamic_element);
    check(!dynamic_result.has_value(), "a non-literal array element is rejected");
    check_eq(first_code(dynamic_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto wrong_element =
        parse(component("  const [rows, setRows] = useState<string[]>([1]);\n"));
    const auto wrong_result = lower(*wrong_element);
    check(!wrong_result.has_value(), "an element literal of the wrong type is rejected");
    check_eq(first_code(wrong_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto nested = parse(component("  const [rows, setRows] = useState<int[]>([[1]]);\n"));
    const auto nested_result = lower(*nested);
    check(!nested_result.has_value(), "a nested array literal is rejected");
    check_eq(first_code(nested_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto record_element =
        parse(component("  const [rows, setRows] = useState<Row[]>([1]);\n"));
    const auto record_result = lower(*record_element);
    check(!record_result.has_value(), "a filled record element type is rejected");
    check_eq(first_code(record_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto non_array_initial = parse(component(
        "  const [rows, setRows] = useState<Row[]>(props.rows);\n", "<View/>", "props: P"));
    const auto non_array_result = lower(*non_array_initial);
    check(non_array_result.has_value(),
          "a list reference is still a valid list initial (the other IR list expression)");

    const auto count_initial =
        parse(component("  const [count, setCount] = useState(0);\n"
                        "  const [rows, setRows] = useState<Row[]>(count);\n"));
    const auto count_result = lower(*count_initial);
    check(
        count_result.has_value(),
        "a reference initial is accepted without type-checking it (expression typing is slice 4)");
}

void test_declared_handler_parameters_need_annotations() {
    const auto inline_handler = parse(component("  const [x, setX] = useState<int>(0);\n"
                                                "  const f = (e) => { setX(e); };\n"));
    const auto inline_result = lower(*inline_handler);
    check(!inline_result.has_value(), "an untyped declared handler parameter is not typed yet");
    check_eq(first_code(inline_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto native_handler = parse(component("  const [x, setX] = useState<int>(0);\n"
                                                "  // Note: bind::go\n"
                                                "  const go = (e) => { setX(1); };\n"));
    const auto native_result = lower(*native_handler);
    check(!native_result.has_value(), "a native handler parameter needs a written type");
    check_eq(first_code(native_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_event_arrow_body_rule() {
    const auto fixture = parse(component("", "<View onClick={() => { doThing(); }} />"));
    const rules::ComponentAnalysis* analysis = analysis_of(*fixture);
    check(analysis != nullptr && analysis->item != nullptr && analysis->item->returned &&
              analysis->item->returned->attributes.size() == 1,
          "one event attribute");
    if (analysis == nullptr || analysis->item == nullptr || !analysis->item->returned ||
        analysis->item->returned->attributes.size() != 1 ||
        !analysis->item->returned->attributes[0].expr) {
        return;
    }
    const lowering::HookLoweringContext context{*analysis->item, *analysis, fixture->map,
                                                "ui/Counter.tsx"};
    const lowering::HandlerActionsResult actions =
        lowering::lower_handler_actions(*analysis->item->returned->attributes[0].expr, context);
    check(!actions.has_value(), "a non-setter statement is not an inline handler body");
    if (!actions.has_value() && !actions.error().empty()) {
        check_eq(std::string(actions.error()[0].code), "TESSERA_IR_UNSUPPORTED", "the code");
        check_eq(std::string(fixture->map.slice(actions.error()[0].span.span)), "doThing()",
                 "the statement span is reported");
    }
}

void test_animated_set_options_are_bounded() {
    const auto fixture =
        parse(component("  const v = useAnimatedValue(1.0);\n",
                        "<View onClick={() => { v.set(1, { animate: false, other: 1 }); }} />"));
    const rules::ComponentAnalysis* analysis = analysis_of(*fixture);
    check(analysis != nullptr && analysis->item != nullptr && analysis->item->returned &&
              analysis->item->returned->attributes.size() == 1,
          "one event attribute");
    if (analysis == nullptr || analysis->item == nullptr || !analysis->item->returned ||
        analysis->item->returned->attributes.size() != 1 ||
        !analysis->item->returned->attributes[0].expr) {
        return;
    }
    const lowering::HookLoweringContext context{*analysis->item, *analysis, fixture->map,
                                                "ui/Counter.tsx"};
    const lowering::HandlerActionsResult actions =
        lowering::lower_handler_actions(*analysis->item->returned->attributes[0].expr, context);
    check(!actions.has_value(), "unknown animated-value setter options are rejected");
    check_eq(first_code(actions), "TESSERA_IR_UNSUPPORTED", "the code");
}

// Timing options must be literals of the right type, each given once.
void test_animated_set_timing_is_literal() {
    const char* const bad[] = {
        "{ duration: -5 }",  "{ duration: \"fast\" }",       "{ duration: speed }", "{ easing: 3 }",
        "{ easing: curve }", "{ duration: 1, duration: 2 }", "{ animate: 1 }",      "{ other: 1 }"};
    for (const char* options : bad) {
        const auto fixture = parse(
            component("  const v = useAnimatedValue(1.0);\n",
                      std::string("<View onClick={() => { v.set(1, ") + options + "); }} />"));
        const rules::ComponentAnalysis* analysis = analysis_of(*fixture);
        if (analysis == nullptr || analysis->item == nullptr || !analysis->item->returned ||
            analysis->item->returned->attributes.size() != 1 ||
            !analysis->item->returned->attributes[0].expr) {
            check(false, std::string("fixture for ") + options);
            continue;
        }
        const lowering::HookLoweringContext context{*analysis->item, *analysis, fixture->map,
                                                    "ui/Counter.tsx"};
        const lowering::HandlerActionsResult actions =
            lowering::lower_handler_actions(*analysis->item->returned->attributes[0].expr, context);
        check(!actions.has_value(), std::string("rejected: ") + options);
    }
}

void test_first_error_only_and_no_partial_ir() {
    const auto fixture = parse(component("  useEffect(() => { go(); }, []);\n"
                                         "  const [x, setX] = useState<{ a: int }>(null);\n"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a component with two errors yields no IR");
    if (!result.has_value()) {
        check(result.error().size() == 1, "only the first error is reported");
        check_eq(first_code(result), "TESSERA_IR_BINDING", "the first error wins");
    }
}

void test_analysis_failure_is_not_lowered() {
    const auto fixture = parse_raw(component("  const [n, setN] = useState<int>(0);\n"
                                             "  useEffect(() => { log(n); }, []);\n"));
    check(!analysis_of(*fixture)->ok, "the fixture fails the analysis pass");
    const auto result = lower(*fixture);
    check(!result.has_value(), "a component the analysis rejected has no IR");
    check_eq(first_code(result), "TESSERA_IR_MALFORMED", "the precondition is reported");
}

void test_mismatched_analysis_is_rejected() {
    const auto first = parse(component(""));
    const auto second = parse(component(""));
    const rules::ComponentAnalysis* analysis = analysis_of(*first);
    check(analysis != nullptr, "the fixture has an analysis");
    if (analysis == nullptr) {
        return;
    }
    if (second->analyzed.components.empty() || second->analyzed.components[0].item == nullptr) {
        check(false, "the second fixture has a component");
        return;
    }
    const lowering::HookLoweringContext context{*second->analyzed.components[0].item, *analysis,
                                                second->map, "ui/Counter.tsx"};
    const auto result = lowering::lower_hooks(context);
    check(!result.has_value(), "an analysis of another component is rejected");
    check_eq(first_code(result), "TESSERA_IR_MALFORMED", "the code");
}

} // namespace

int main() {
    test_effect_needs_a_binding();
    test_state_type_needs_the_type_table();
    test_array_list_initial_rejections();
    test_declared_handler_parameters_need_annotations();
    test_event_arrow_body_rule();
    test_animated_set_options_are_bounded();
    test_animated_set_timing_is_literal();
    test_first_error_only_and_no_partial_ir();
    test_analysis_failure_is_not_lowered();
    test_mismatched_analysis_is_rejected();
    return finish("lowering hooks reject tests");
}
