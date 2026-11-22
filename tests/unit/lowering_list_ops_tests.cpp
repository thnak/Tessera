// SPDX-License-Identifier: MIT

// The list and string expressions of ADR-066 item 3: `filter`, `slice`, `.length` as `length(...)`,
// the total string methods, and the record spread `{ ...state, field: value }`. The lowering half
// is here; `codegen_list_ops_tests` covers the generated C++ and the evaluator half is in
// `ir_eval_list_ops_tests`.

#include "lowering_hooks_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace lowering_hooks_test;

// A component with one `useState` list and one handler whose body is `body`.
std::string component_with_list_handler(std::string_view body) {
    std::string source = "function A({ rows }: { rows: string[] }) {\n"
                         "  const [items, setItems] = useState<string[]>([]);\n"
                         "  const act = () => {\n";
    source += body;
    source += "  };\n"
              "  return <View onClick={act} />;\n"
              "}";
    return source;
}

void test_filter_lowers_to_list_filter() {
    const auto fixture = parse(component_with_list_handler("    setItems(rows.filter(r => r));\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    const std::string text = describe(*lowered);
    check(text.find("listFilter") != std::string::npos ||
              text.find("\"kind\":\"listFilter\"") != std::string::npos,
          "the value is a list filter: " + text);
}

void test_slice_lowers_to_list_slice() {
    const auto fixture = parse(component_with_list_handler("    setItems(rows.slice(0, 2));\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    const std::string text = describe(*lowered);
    check(text.find("listSlice") != std::string::npos ||
              text.find("\"kind\":\"listSlice\"") != std::string::npos,
          "the value is a list slice: " + text);
}

void test_length_is_the_pure_function() {
    // `.length` is `length(list)`, not a member read, so it lowers to a call.
    const auto fixture =
        parse(component_with_list_handler("    setItems(rows.slice(0, length(rows)));\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    check(describe(*lowered).find("\"callee\":\"length\"") != std::string::npos,
          "`length` is a call: " + describe(*lowered));
}

void test_string_methods_lower_to_calls() {
    // A string method takes its receiver as the first argument, so a method call and a function
    // call are one IR shape.
    const auto fixture = parse("function A({ name }: { name: string }) {\n"
                               "  return <View label={name.trim().toUpperCase()} />;\n"
                               "}");
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the component lowers: " + describe_errors(*fixture, lowered));
}

void test_filter_predicate_must_be_an_expression() {
    // A block body has no IR form: the IR carries the item name and one expression.
    const auto fixture =
        parse(component_with_list_handler("    setItems(rows.filter(r => { return r; }));\n"));
    const auto lowered = lower(*fixture);
    check(!lowered.has_value(), "a block predicate is rejected");
}

void test_filter_needs_one_parameter() {
    const auto fixture =
        parse(component_with_list_handler("    setItems(rows.filter(() => true));\n"));
    const auto lowered = lower(*fixture);
    check(!lowered.has_value(), "a predicate with no parameter is rejected");
}

void test_unknown_method_is_rejected() {
    // `sort` is still a parser rejection, because it mutates its receiver.
    const auto parsed =
        jsx::parse_module(component_with_list_handler("    setItems(rows.sort());\n"));
    check(!parsed.ok(), "an unsupported method is rejected");
    check_eq(codes(parsed), "TESSERA_JSX_SUBSET_ARRAY_METHOD;", "the code");
}

} // namespace

int main() {
    test_filter_lowers_to_list_filter();
    test_slice_lowers_to_list_slice();
    test_length_is_the_pure_function();
    test_string_methods_lower_to_calls();
    test_filter_predicate_must_be_an_expression();
    test_filter_needs_one_parameter();
    test_unknown_method_is_rejected();
    return finish("lowering list ops tests");
}
