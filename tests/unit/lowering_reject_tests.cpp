// SPDX-License-Identifier: MIT

// Every AST expression that has no IR form is rejected with a TESSERA_IR_* diagnostic, and a
// failed expression produces no partial IR (docs/design/jsx-ir-lowering.md sections 4 and 9).

#include "lowering_test_util.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace lowering_test;

const MapNames kNames{
    {"a", "state::a"},       {"b", "state::b"},   {"count", "state::count"},
    {"rows", "state::rows"}, {"ok", "state::ok"},
};

std::unique_ptr<jsx::Expr> identifier(std::string name, std::uint32_t offset) {
    auto expr = std::make_unique<jsx::Expr>();
    expr->kind = jsx::ExprKind::Identifier;
    expr->span = jsx::Span{offset, static_cast<std::uint32_t>(name.size())};
    expr->text = std::move(name);
    return expr;
}

// Lowers a hand-built AST against `source`, which only gives the spans a file to point at.
lowering::ExpressionLoweringResult lower_ast(const jsx::Expr& expr, const std::string& source) {
    const jsx::SourceMap map(source);
    const lowering::ExpressionLoweringContext context{map, "ui/Counter.tsx", &kNames};
    return lowering::lower_expression(expr, context);
}

void test_aggregate_and_closure_literals_are_rejected() {
    const Lowered array = lower("[1, 2]", &kNames);
    check(!array.result, "an array literal produces no IR");
    check_eq(first_code(array.result), "TESSERA_IR_UNSUPPORTED", "array literal code");
    check(!array.result && array.result.error().size() == 1,
          "one diagnostic for one unsupported expression");
    if (!array.result) {
        const jsx::Diagnostic diagnostic = first_diagnostic(array.result);
        check(diagnostic.span.span.offset == 0 && diagnostic.span.span.length == 6,
              "the array diagnostic covers the whole literal");
        check(diagnostic.span.start.line == 1 && diagnostic.span.start.column == 1 &&
                  diagnostic.span.end.column == 7,
              "the array diagnostic has 1-based columns");
    }

    check_eq(first_code(lower("{ a: 1 }", &kNames).result), "TESSERA_IR_UNSUPPORTED",
             "object literal code");
    check_eq(first_code(lower("() => 1", &kNames).result), "TESSERA_IR_UNSUPPORTED", "arrow code");
    check_eq(first_code(lower("<View/>", &kNames).result), "TESSERA_IR_UNSUPPORTED",
             "JSX inside an expression code");
}

void test_assignment_is_rejected() {
    const Lowered assignment = lower("x = 1", &kNames);
    check(!assignment.result, "an assignment produces no IR");
    check_eq(first_code(assignment.result), "TESSERA_IR_UNSUPPORTED", "assignment code");
    if (!assignment.result) {
        check_eq(std::string(first_diagnostic(assignment.result).code), "TESSERA_IR_UNSUPPORTED",
                 "assignment diagnostic code");
    }
}

void test_member_and_call_forms_are_rejected() {
    const Lowered index = lower("rows[0]", &kNames);
    check(!index.result, "indexing produces no IR");
    check_eq(first_code(index.result), "TESSERA_IR_UNSUPPORTED", "indexing code");

    const Lowered method = lower("Math.max(1, 2)", &kNames);
    check(!method.result, "a method call produces no IR");
    check_eq(first_code(method.result), "TESSERA_IR_UNSUPPORTED", "method call code");

    const Lowered unknown = lower("foo(1)", &kNames);
    check(!unknown.result, "an unknown function produces no IR");
    check_eq(first_code(unknown.result), "TESSERA_IR_UNSUPPORTED", "unknown function code");
    if (!unknown.result) {
        const jsx::Diagnostic diagnostic = first_diagnostic(unknown.result);
        check(diagnostic.span.span.offset == 0 && diagnostic.span.span.length == 3,
              "the call diagnostic points at the callee");
        check(diagnostic.suggestion.find("formatNumber") != std::string::npos,
              "the call diagnostic lists the allowlist");
    }

    const Lowered hook = lower("useState(0)", &kNames);
    check(!hook.result, "a hook call is not a pure expression");
    check_eq(first_code(hook.result), "TESSERA_IR_UNSUPPORTED", "hook call code");

    const Lowered member_on_literal = lower("\"x\".length", &kNames);
    check(!member_on_literal.result, "member access on a literal produces no IR");
    check_eq(first_code(member_on_literal.result), "TESSERA_IR_UNSUPPORTED",
             "member on a literal code");
}

void test_unresolved_references() {
    const Lowered no_resolver = lower("count");
    check(!no_resolver.result, "a reference without a resolver produces no IR");
    check_eq(first_code(no_resolver.result), "TESSERA_IR_UNRESOLVED", "no resolver code");

    const Lowered unknown = lower("missing", &kNames);
    check(!unknown.result, "an unknown name produces no IR");
    check_eq(first_code(unknown.result), "TESSERA_IR_UNRESOLVED", "unknown name code");
    if (!unknown.result) {
        const jsx::Diagnostic diagnostic = first_diagnostic(unknown.result);
        check(diagnostic.span.span.offset == 0 && diagnostic.span.span.length == 7,
              "the unresolved diagnostic covers the name");
    }
}

void test_integer_literals_outside_int32() {
    const Lowered too_large = lower("2147483648");
    check(!too_large.result, "an integer outside int32 produces no IR");
    check(expr_of(too_large) == nullptr, "no partial IR is returned for an out-of-range integer");
    check_eq(first_code(too_large.result), "TESSERA_IR_LITERAL", "out-of-range integer code");
    if (!too_large.result) {
        check(too_large.result.error().size() == 1, "one diagnostic for the literal");
        const jsx::Diagnostic diagnostic = first_diagnostic(too_large.result);
        check(diagnostic.span.span.offset == 0 && diagnostic.span.span.length == 10,
              "the diagnostic covers the literal");
        check(diagnostic.suggestion.find("2147483648.0") != std::string::npos,
              "the diagnostic suggests the float spelling");
    }

    // The minimum-int fold only covers the negated value that fits; one below it stays an error.
    const Lowered below_minimum = lower("-2147483649");
    check(!below_minimum.result, "an integer below the minimum int produces no IR");
    check_eq(first_code(below_minimum.result), "TESSERA_IR_LITERAL", "below-minimum integer code");

    const Lowered nested = lower("2147483648 + 1");
    check(!nested.result, "an out-of-range literal fails the whole expression");
    check(expr_of(nested) == nullptr, "a failed tree returns no IR");
}

void test_hand_built_loose_equality_is_rejected() {
    jsx::Expr binary;
    binary.kind = jsx::ExprKind::Binary;
    binary.text = "==";
    binary.span = jsx::Span{0, 6};
    binary.operands.push_back(identifier("a", 0));
    binary.operands.push_back(identifier("b", 5));

    const auto result = lower_ast(binary, "a == b");
    check(!result, "loose equality produces no IR");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "loose equality code");
    if (!result) {
        check_eq(std::string(first_diagnostic(result).suggestion), "use `===` or `!==`",
                 "loose equality suggests strict equality");
    }
}

void test_hand_built_unsupported_unary_is_rejected() {
    jsx::Expr unary;
    unary.kind = jsx::ExprKind::Unary;
    unary.text = "await";
    unary.span = jsx::Span{0, 11};
    unary.operands.push_back(identifier("count", 6));

    const auto result = lower_ast(unary, "await count");
    check(!result, "`await` produces no IR");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "await code");
}

void test_malformed_hand_built_asts() {
    jsx::Expr binary;
    binary.kind = jsx::ExprKind::Binary;
    binary.text = "+";
    binary.span = jsx::Span{0, 1};
    binary.operands.push_back(identifier("a", 0));
    check_eq(first_code(lower_ast(binary, "a")), "TESSERA_IR_MALFORMED",
             "a binary with one operand is malformed");

    jsx::Expr conditional;
    conditional.kind = jsx::ExprKind::Conditional;
    conditional.span = jsx::Span{0, 1};
    check_eq(first_code(lower_ast(conditional, "a")), "TESSERA_IR_MALFORMED",
             "a conditional without operands is malformed");

    jsx::Expr template_expr;
    template_expr.kind = jsx::ExprKind::Template;
    template_expr.span = jsx::Span{0, 1};
    template_expr.template_strings = {"a", "b"};
    check_eq(first_code(lower_ast(template_expr, "a")), "TESSERA_IR_MALFORMED",
             "a template with mismatched parts is malformed");

    jsx::Expr empty_name;
    empty_name.kind = jsx::ExprKind::Identifier;
    empty_name.span = jsx::Span{0, 0};
    check_eq(first_code(lower_ast(empty_name, "a")), "TESSERA_IR_MALFORMED",
             "an empty identifier is malformed");
}

void test_failure_never_returns_partial_ir() {
    const Lowered nested = lower("1 + [2]", &kNames);
    check(!nested.result, "a nested unsupported expression fails the whole tree");
    if (!nested.result) {
        check(nested.result.error().size() == 1, "the failure reports one diagnostic");
        const jsx::Diagnostic diagnostic = first_diagnostic(nested.result);
        check(diagnostic.span.span.offset == 4 && diagnostic.span.span.length == 3,
              "the diagnostic points into the failing subtree");
    }
}

} // namespace

int main() {
    test_aggregate_and_closure_literals_are_rejected();
    test_assignment_is_rejected();
    test_member_and_call_forms_are_rejected();
    test_unresolved_references();
    test_integer_literals_outside_int32();
    test_hand_built_loose_equality_is_rejected();
    test_hand_built_unsupported_unary_is_rejected();
    test_malformed_hand_built_asts();
    test_failure_never_returns_partial_ir();
    return finish("lowering rejection tests");
}
