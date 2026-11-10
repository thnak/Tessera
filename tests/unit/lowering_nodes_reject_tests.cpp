// SPDX-License-Identifier: MIT

// Slice 7 rejection cases: unsupported nodes, unknown props and events, slice boundaries (component
// references 9, conditionals and loops 8, custom elements 11, types 4), and the first-error-only /
// no-partial-component rule.

#include "lowering_nodes_test_util.hpp"

namespace {

using namespace lowering_nodes_test;

void test_component_reference_is_slice_9() {
    const auto fixture = parse(component("", "<View><Counter /></View>"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a component reference has no IR form yet");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_root_fragment_is_rejected() {
    const auto fixture = parse(component("", "<><View /><View /></>"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a component cannot return a fragment");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_unknown_props_and_events() {
    const auto prop = parse(component("", "<View nope=\"x\" />"));
    const auto prop_result = lower(*prop);
    check(!prop_result.has_value(), "an unknown prop is rejected");
    check_eq(first_code(prop_result), "TESSERA_IR_UNKNOWN_PROP", "the unknown-prop code");

    const auto event = parse(component("  const [n, setN] = useState<int>(0);\n",
                                       "<View onDoubleClick={() => setN(1)} />"));
    const auto event_result = lower(*event);
    check(!event_result.has_value(), "an unknown event is rejected");
    check_eq(first_code(event_result), "TESSERA_IR_UNKNOWN_PROP", "the unknown-event code");

    const auto aria = parse(component("", "<View aria-labelledby=\"x\" />"));
    const auto aria_result = lower(*aria);
    check(!aria_result.has_value(), "an unknown aria-* name is rejected");
    check_eq(first_code(aria_result), "TESSERA_IR_UNKNOWN_PROP", "the code");
}

void test_prop_restrictions_from_the_schema() {
    const auto rows = parse(component("", "<TextInput rows={2} />"));
    const auto rows_result = lower(*rows);
    check(!rows_result.has_value(), "`rows` is TextArea-only");
    check_eq(first_code(rows_result), "TESSERA_IR_UNKNOWN_PROP", "the code");

    const auto secure = parse(component("", "<TextArea secure />"));
    const auto secure_result = lower(*secure);
    check(!secure_result.has_value(), "`secure` is TextInput-only");
    check_eq(first_code(secure_result), "TESSERA_IR_UNKNOWN_PROP", "the code");

    const auto class_name = parse(component("", "<Spacer grow={1} className=\"x\" />"));
    const auto class_name_result = lower(*class_name);
    check(!class_name_result.has_value(), "className is not accepted on Spacer");
    check_eq(first_code(class_name_result), "TESSERA_IR_UNKNOWN_PROP", "the code");

    const auto style =
        parse(component("", "<ErrorBoundary fallback={<View />} style={{ width: 1 }} />"));
    const auto style_result = lower(*style);
    check(!style_result.has_value(), "style is not accepted on ErrorBoundary");
    check_eq(first_code(style_result), "TESSERA_IR_UNKNOWN_PROP", "the code");
}

void test_ref_rules() {
    const auto refused = parse(component("", "<Spacer grow={1} ref={r} />"));
    const auto refused_result = lower(*refused);
    check(!refused_result.has_value(), "Spacer rejects ref");
    check_eq(first_code(refused_result), "TESSERA_IR_UNKNOWN_PROP", "the code");

    const auto unknown = parse(component("", "<View ref={r} />"));
    const auto unknown_result = lower(*unknown);
    check(!unknown_result.has_value(), "an undeclared ref name is rejected");
    check_eq(first_code(unknown_result), "TESSERA_IR_UNRESOLVED", "the code");

    const auto not_a_name =
        parse(component("  const r = useRef<View>(null);\n", "<View ref={() => r} />"));
    const auto not_a_name_result = lower(*not_a_name);
    check(!not_a_name_result.has_value(), "a callback ref is rejected");
    check_eq(first_code(not_a_name_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_automation_id_and_style_forms() {
    const auto automation = parse(
        component("  const [n, setN] = useState<string>(\"1\");\n", "<View automationId={n} />"));
    const auto automation_result = lower(*automation);
    check(!automation_result.has_value(), "automationId must be a string literal");
    check_eq(first_code(automation_result), "TESSERA_IR_UNSUPPORTED", "the code");

    // A non-object `style` is already TESSERA_JSX_SUBSET_STYLE_DYNAMIC at parse time; the lowering
    // check for it is defensive and unreachable from parsed source.

    const auto style_property = parse(component("", "<View style={{ nope: 1 }} />"));
    const auto style_property_result = lower(*style_property);
    check(!style_property_result.has_value(), "an unknown style property is rejected");
    check_eq(first_code(style_property_result), "TESSERA_IR_UNKNOWN_PROP", "the code");
}

void test_event_value_forms() {
    const auto literal = parse(component("  const [count, setCount] = useState<int>(0);\n",
                                         "<Button onClick={count}>Go</Button>"));
    const auto literal_result = lower(*literal);
    check(!literal_result.has_value(), "a non-handler event value is rejected");
    check_eq(first_code(literal_result), "TESSERA_IR_UNRESOLVED", "the code");

    const auto expression = parse(component("  const [count, setCount] = useState<int>(0);\n",
                                            "<Button onClick={1 + 2}>Go</Button>"));
    const auto expression_result = lower(*expression);
    check(!expression_result.has_value(), "an expression is not a handler");
    check_eq(first_code(expression_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto parameters = parse(component("  const [count, setCount] = useState<int>(0);\n",
                                            "<Button onClick={(a, b) => setCount(1)}>Go</Button>"));
    const auto parameters_result = lower(*parameters);
    check(!parameters_result.has_value(), "a handler takes at most one parameter");
    check_eq(first_code(parameters_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto payload = parse(component("  const [count, setCount] = useState<int>(0);\n",
                                         "<VirtualList onEndReached={setCount} />"));
    const auto payload_result = lower(*payload);
    check(!payload_result.has_value(), "a setter needs a payload record");
    check_eq(first_code(payload_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_spread_of_the_props_parameter() {
    const auto fixture = parse(component("", "<View {...props} />", "props: P"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a props spread needs the props type");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_literal_only_props() {
    const auto fixture = parse(component("", "<Image src={props.a} alt=\"a\" />", "props: P"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "Image src is literal-only");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_children_rules() {
    const auto leaf = parse(component("", "<TextInput value=\"x\">oops</TextInput>"));
    const auto leaf_result = lower(*leaf);
    check(!leaf_result.has_value(), "a leaf accepts no children");
    check_eq(first_code(leaf_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto text_element = parse(component("", "<Text><View /></Text>"));
    const auto text_element_result = lower(*text_element);
    check(!text_element_result.has_value(), "Text accepts no element child");
    check_eq(first_code(text_element_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto scroll_text = parse(component("", "<Scroll>text</Scroll>"));
    const auto scroll_text_result = lower(*scroll_text);
    check(!scroll_text_result.has_value(), "Scroll accepts nodes only");
    check_eq(first_code(scroll_text_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto boundary_missing = parse(component("", "<ErrorBoundary fallback={<View />} />"));
    const auto boundary_missing_result = lower(*boundary_missing);
    check(!boundary_missing_result.has_value(), "ErrorBoundary needs one child");
    check_eq(first_code(boundary_missing_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto boundary_two =
        parse(component("", "<ErrorBoundary fallback={<View />}><View /><View /></ErrorBoundary>"));
    const auto boundary_two_result = lower(*boundary_two);
    check(!boundary_two_result.has_value(), "ErrorBoundary takes exactly one child");
    check_eq(first_code(boundary_two_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto boundary_fragment =
        parse(component("", "<ErrorBoundary fallback={<View />}><><View /></></ErrorBoundary>"));
    const auto boundary_fragment_result = lower(*boundary_fragment);
    check(!boundary_fragment_result.has_value(), "ErrorBoundary rejects a fragment child");
    check_eq(first_code(boundary_fragment_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_later_slice_boundaries() {
    // `.map` loops and virtual item templates are implemented; a bare virtual collection without
    // its template is the remaining boundary case (ir-v1 section 3.5).
    const auto virtual_list = parse(component("", "<VirtualList itemSize={10} />"));
    const auto virtual_list_result = lower(*virtual_list);
    check(!virtual_list_result.has_value(),
          "a virtualized collection needs its `.map` item template");
    check_eq(first_code(virtual_list_result), "TESSERA_IR_VIRTUAL", "the code");

    const auto custom = parse(component("", "<CustomElement element=\"Waterfall\" />"));
    const auto custom_result = lower(*custom);
    check(!custom_result.has_value(), "CustomElement is slice 11");
    check_eq(first_code(custom_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_virtual_collection_event_rejections() {
    // `onEndReached` has no payload in the schema (elements_virtual_data.hpp): a parameter is an
    // error rather than a silently dropped payload.
    const auto payload = parse(component(
        "  const [n, setN] = useState<int>(0);\n",
        "<VirtualList itemSize={32} onEndReached={(e) => setN(1)}>{props.rows.map((row) => "
        "<Text key={row.id}>{row.label}</Text>)}</VirtualList>",
        "props: P"));
    const auto payload_result = lower(*payload);
    check(!payload_result.has_value(), "onEndReached rejects a payload parameter");
    check_eq(first_code(payload_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto parameters = parse(component(
        "  const [n, setN] = useState<int>(0);\n",
        "<VirtualList itemSize={32} onScroll={(a, b) => setN(1)}>{props.rows.map((row) => "
        "<Text key={row.id}>{row.label}</Text>)}</VirtualList>",
        "props: P"));
    const auto parameters_result = lower(*parameters);
    check(!parameters_result.has_value(), "an event handler takes at most one parameter");
    check_eq(first_code(parameters_result), "TESSERA_IR_UNSUPPORTED", "the code");

    // `onScroll` is a collection event, not a View event.
    const auto view = parse(
        component("  const [n, setN] = useState<int>(0);\n", "<View onScroll={(e) => setN(1)} />"));
    const auto view_result = lower(*view);
    check(!view_result.has_value(), "onScroll is not an event of View");
    check_eq(first_code(view_result), "TESSERA_IR_UNKNOWN_PROP", "the code");
}

void test_first_error_only_and_no_partial_component() {
    const auto fixture = parse(component(
        "", "<View>\n  <Counter />\n  <Image src={props.a} alt=\"a\" />\n</View>", "props: P"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a component with several errors yields no IR");
    if (!result.has_value()) {
        check(result.error().size() == 1, "only the first error is reported");
        check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the first error wins");
    }
}

void test_analysis_failure_is_not_lowered() {
    const auto fixture = parse_raw(component("  const [n, setN] = useState<int>(0);\n"
                                             "  useEffect(() => { log(n); }, []);\n",
                                             "<View />"));
    check(!analysis_of(*fixture)->ok, "the fixture fails the analysis pass");
    const auto result = lower(*fixture);
    check(!result.has_value(), "a component the analysis rejected has no IR");
    check_eq(first_code(result), "TESSERA_IR_MALFORMED", "the precondition is reported");
}

} // namespace

int main() {
    test_component_reference_is_slice_9();
    test_root_fragment_is_rejected();
    test_unknown_props_and_events();
    test_prop_restrictions_from_the_schema();
    test_ref_rules();
    test_automation_id_and_style_forms();
    test_event_value_forms();
    test_spread_of_the_props_parameter();
    test_literal_only_props();
    test_children_rules();
    test_later_slice_boundaries();
    test_virtual_collection_event_rejections();
    test_first_error_only_and_no_partial_component();
    test_analysis_failure_is_not_lowered();
    return finish("lowering nodes reject tests");
}
