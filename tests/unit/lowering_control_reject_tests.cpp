// SPDX-License-Identifier: MIT

// Slice 8 rejection cases: malformed conditionals, loop sources, nesting depth, attribute position,
// children rules of Text/ErrorBoundary/leaf elements, and first-error-only behaviour.
//
// The parser already rejects, before lowering, the loop forms that are not the accepted shape
// (subset 10.5): `async` callbacks, destructuring callback parameters, a non-arrow or additional
// callback argument, zero or three parameters, a block or non-element body, a missing `key`, and
// every array method other than `.map` (`flatMap`, `filter`, ...). The matching checks in the
// lowering pass are defensive and cannot be reached from parsed source; they are noted here so the
// boundary is explicit.

#include "lowering_nodes_test_util.hpp"

namespace {

using namespace lowering_nodes_test;

void test_loop_source_must_be_a_name() {
    // The parser checks the callback shape and the key, but not the receiver form.
    const auto call = parse(component(
        "", "<View>{min(1, 2).map((row) => <Text key={row.id}>a</Text>)}</View>", "props: P"));
    const auto call_result = lower(*call);
    check(!call_result.has_value(), "a loop source must be a list name");
    check_eq(first_code(call_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_nested_loop_depth() {
    const auto depth_three = parse(
        component("",
                  "<View>{props.rows.map((row) => <View key={row.id}>{row.children.map((child) => "
                  "<View key={child.id}>{child.grand.map((g) => <Text key={g.id} />)}</View>)"
                  "}</View>)}</View>",
                  "props: P"));
    const auto result = lower(*depth_three);
    check(!result.has_value(), "nested loops beyond depth 2 are rejected");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_conditional_forms() {
    const auto mixed = parse(component("  const [flag, setFlag] = useState(true);\n",
                                       "<View>{flag ? <Text>a</Text> : 1}</View>"));
    const auto mixed_result = lower(*mixed);
    check(!mixed_result.has_value(), "the other branch must be null or false");
    check_eq(first_code(mixed_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto fragment = parse(component("  const [flag, setFlag] = useState(true);\n",
                                          "<View>{flag ? <><Text>a</Text></> : null}</View>"));
    const auto fragment_result = lower(*fragment);
    check(!fragment_result.has_value(), "a fragment branch is rejected");
    check_eq(first_code(fragment_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto or_form = parse(component("  const [flag, setFlag] = useState(true);\n",
                                         "<View>{flag || <Text>a</Text>}</View>"));
    const auto or_form_result = lower(*or_form);
    check(!or_form_result.has_value(), "only `&&` and `?:` render conditionally");
    check_eq(first_code(or_form_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_control_in_attribute_position() {
    const auto loop = parse(
        component("", "<ErrorBoundary fallback={props.rows.map((row) => <View key={row.id} />)} />",
                  "props: P"));
    const auto loop_result = lower(*loop);
    check(!loop_result.has_value(), "a loop cannot be an attribute value");
    check_eq(first_code(loop_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto conditional =
        parse(component("  const [flag, setFlag] = useState(true);\n",
                        "<ErrorBoundary fallback={flag ? <View /> : <View />} />"));
    const auto conditional_result = lower(*conditional);
    check(!conditional_result.has_value(), "a conditional cannot be an attribute value");
    check_eq(first_code(conditional_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_control_in_wrong_child_slots() {
    const auto text = parse(component("  const [flag, setFlag] = useState(true);\n",
                                      "<Text>{flag && <Text>a</Text>}</Text>"));
    const auto text_result = lower(*text);
    check(!text_result.has_value(), "Text accepts no conditional child");
    check_eq(first_code(text_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto boundary =
        parse(component("  const [flag, setFlag] = useState(true);\n",
                        "<ErrorBoundary fallback={<View />}>{flag && <View />}</ErrorBoundary>"));
    const auto boundary_result = lower(*boundary);
    check(!boundary_result.has_value(), "ErrorBoundary accepts no conditional child");
    check_eq(first_code(boundary_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto boundary_loop = parse(component(
        "",
        "<ErrorBoundary fallback={<View />}>{props.rows.map((row) => <View key={row.id} />)}"
        "</ErrorBoundary>",
        "props: P"));
    const auto boundary_loop_result = lower(*boundary_loop);
    check(!boundary_loop_result.has_value(), "ErrorBoundary accepts no loop child");
    check_eq(first_code(boundary_loop_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto leaf =
        parse(component("",
                        "<Image src=\"a.png\" alt=\"\">{props.rows.map((row) => <View key={row.id} "
                        "/>)}</Image>",
                        "props: P"));
    const auto leaf_result = lower(*leaf);
    check(!leaf_result.has_value(), "a leaf accepts no loop child");
    check_eq(first_code(leaf_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_first_error_only_and_no_partial_component() {
    const auto fixture =
        parse(component("",
                        "<View>{min(1, 2).map((row) => <Text key={row.id}>a</Text>)}"
                        "<Image src={props.a} alt=\"a\" /></View>",
                        "props: P"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a component with several errors yields no IR");
    if (!result.has_value()) {
        check(result.error().size() == 1, "only the first error is reported");
        check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the first error wins");
    }
}

} // namespace

int main() {
    test_loop_source_must_be_a_name();
    test_nested_loop_depth();
    test_conditional_forms();
    test_control_in_attribute_position();
    test_control_in_wrong_child_slots();
    test_first_error_only_and_no_partial_component();
    return finish("lowering control reject tests");
}
