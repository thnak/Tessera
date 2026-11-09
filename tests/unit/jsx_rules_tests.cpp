// SPDX-License-Identifier: MIT

// Rules decided from syntax alone: list loops, array methods, `style`, spread
// (docs/specs/jsx-subset-v1.md sections 10.2, 10.3, 10.5, 12).

#include "jsx_test_util.hpp"

#include <string>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

std::string codes_of_expr(const std::string& source) {
    return codes(parse_expression(source));
}

void test_map_list_form() {
    const auto good = parse_expression("rows.map((r, i) => (<Row key={r.id} n={i}/>))");
    check(good.ok(), "valid list loop: " + codes(good));
    check_eq(codes_of_expr("rows.map(r => <Row key={r.id}/>)"), "", "single bare parameter");
    check_eq(codes_of_expr("rows.map((r) => <Row key=\"k\"/>)"), "", "string key is a key");
}

void test_map_missing_key() {
    const auto result = parse_expression("rows.map((r) => <Row id={r.id}/>)");
    check_eq(codes(result), "TESSERA_REACT_LIST_KEY;", "no key attribute");
    check_eq(first_diag(result).suggestion, "add `key={item.id}`", "suggestion");
    check_eq(codes_of_expr("rows.map((r) => <Row key/>)"), "TESSERA_REACT_LIST_KEY;",
             "a bare `key` flag is not a key");
}

void test_map_bad_shapes() {
    for (const char* source :
         {"rows.map(f)", "rows.map()", "rows.map((r) => <A key={r}/>, 1)",
          "rows.map(() => <A key={1}/>)", "rows.map((a, b, c) => <A key={a}/>)",
          "rows.map((r) => { return <A key={r}/>; })", "rows.map((r) => { <A key={r}/> })",
          "rows.map((r) => r.name)", "rows.map((r) => <>text</>)",
          "rows.map((r) => (r ? <A key={r}/> : null))"}) {
        check_eq(codes_of_expr(source), "TESSERA_JSX_SUBSET_CALL;", source);
    }
}

void test_array_methods() {
    for (const char* method : {"filter", "reduce", "forEach", "sort", "slice", "find", "push",
                               "join", "includes", "flatMap", "at"}) {
        const auto result = parse_expression(std::string("rows.") + method + "(x)");
        check_eq(codes(result), "TESSERA_JSX_SUBSET_ARRAY_METHOD;", method);
    }
    check_eq(codes_of_expr("a.set(1)"), "", "`.set` is not an array method");
    check_eq(codes_of_expr("a.toString()"), "", "`.toString` is not rejected");
    check_eq(codes_of_expr("a.mapper(f)"), "", "only the exact name `map` is checked");
    check_eq(codes_of_expr("rows[0](x)"), "TESSERA_JSX_SUBSET_MEMBER;",
             "computed access: only the member error");
    check_eq(codes_of_expr("filter(x)"), "", "a plain function named filter is not a method");
}

void test_style_rules() {
    check_eq(codes(parse_jsx_element("<A style={{ width: 10, backgroundColor: c }}/>")), "",
             "inline object with identifier keys");
    for (const char* source :
         {"<A style={s}/>", "<A style=\"width: 10px\"/>", "<A style={{ \"width\": 10 }}/>",
          "<A style={cond ? a : b}/>", "<A style/>"}) {
        check_eq(codes(parse_jsx_element(source)), "TESSERA_JSX_SUBSET_STYLE_DYNAMIC;", source);
    }
    check_eq(codes(parse_jsx_element("<A styles={s}/>")), "", "only the `style` attribute");
}

void test_spread_rules() {
    const auto in_component = [](const char* attribute) {
        return parse_module(std::string("function A(props: P) { return <B ") + attribute +
                            "/>; }\n");
    };
    check(in_component("{...props}").ok(), "spreading the props parameter");
    check(in_component("{...{ a: 1 }}").ok(), "spreading an object literal");
    check_eq(codes(in_component("{...other}")), "TESSERA_JSX_SUBSET_SPREAD;", "another name");
    check_eq(codes(in_component("{...props.rest}")), "TESSERA_JSX_SUBSET_SPREAD;", "a member");
    check_eq(codes(in_component("{...f()}")), "TESSERA_JSX_SUBSET_SPREAD;", "a call");

    const auto destructured = parse_module("function A({ a }: P) { return <B {...a}/>; }\n");
    check_eq(codes(destructured), "TESSERA_JSX_SUBSET_SPREAD;",
             "destructured props have no props object to spread");

    check_eq(codes(parse_jsx_element("<B {...anything}/>")), "",
             "outside a component the rule has no props name and is not applied");
}

void test_typed_native_handler_makes_valid_component() {
    const auto result =
        parse_module("function Pick(props: P) {\n"
                     "  // Note: bind::on_pick\n"
                     "  const pick = (id: int, name: string): void => { go(id); };\n"
                     "  return <B onTap={pick} {...props}/>;\n"
                     "}\n");
    check(result.ok(), "typed handler component is valid: " + codes(result));
    check_eq(validity_text(result), "V", "item valid");
}

} // namespace

int main() {
    test_map_list_form();
    test_map_missing_key();
    test_map_bad_shapes();
    test_array_methods();
    test_style_rules();
    test_spread_rules();
    test_typed_native_handler_makes_valid_component();
    return finish("jsx rules tests");
}
