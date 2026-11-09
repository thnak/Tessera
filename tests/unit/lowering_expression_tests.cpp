// SPDX-License-Identifier: MIT

// Expression lowering of the supported forms: literals, refs, unary, binary, conditional,
// template and call (docs/design/jsx-ir-lowering.md section 9, slice 5).

#include "lowering_test_util.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace lowering_test;

const MapNames kNames{
    {"a", "state::a"},         {"b", "state::b"},
    {"c", "state::c"},         {"s", "state::s"},
    {"count", "state::count"}, {"ok", "state::ok"},
    {"title", "state::title"}, {"props.title", "prop::title"},
    {"item.id", "item.id"},    {"item.row.id", "item.row.id"},
};

bool is_int(const ir::Expr* expr, std::int64_t expected) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return false;
    }
    const auto* value = std::get_if<std::int64_t>(&expr->literal);
    return value != nullptr && *value == expected;
}

bool is_float(const ir::Expr* expr, double expected) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return false;
    }
    const auto* value = std::get_if<double>(&expr->literal);
    return value != nullptr && *value == expected;
}

bool lowers_to_int(std::string_view source, std::int64_t expected) {
    const Lowered lowered = lower(source);
    return is_int(expr_of(lowered), expected);
}

bool lowers_to_float(std::string_view source, double expected) {
    const Lowered lowered = lower(source);
    return is_float(expr_of(lowered), expected);
}

void test_integer_and_float_literals() {
    check(lowers_to_int("0", 0), "0 is an int literal");
    check(lowers_to_int("42", 42), "42 is an int literal");
    check(lowers_to_int("2147483647", 2147483647), "the largest int stays an int");
    // The spec leaves literal typing open; slice 5 widens an integer outside int32 to float
    // because the IR has no wider int (ir-v1.md section 4.1, documented in the design note).
    check(lowers_to_float("2147483648", 2147483648.0), "an integer outside int32 becomes a float");
    check(lowers_to_float("0.5", 0.5), "a fraction is a float");
    check(lowers_to_float("1e3", 1000.0), "an exponent spelling is a float");
    check(lowers_to_float("1.0", 1.0), "1.0 keeps the float spelling");
}

void test_string_boolean_and_null_literals() {
    const Lowered lowered = lower("\"a\\nb\"");
    const ir::Expr* string = expr_of(lowered);
    check(string != nullptr && string->kind == ir::ExprKind::Literal, "a string lowers");
    if (string != nullptr) {
        const auto* value = std::get_if<std::string>(&string->literal);
        check(value != nullptr && *value == "a\nb", "the string literal is already decoded");
    }

    const Lowered boolean_lowered = lower("true");
    const ir::Expr* boolean = expr_of(boolean_lowered);
    check(boolean != nullptr && std::get_if<bool>(&boolean->literal) != nullptr &&
              std::get<bool>(boolean->literal),
          "true is a bool literal");

    const Lowered null_lowered = lower("null");
    const ir::Expr* null_value = expr_of(null_lowered);
    check(null_value != nullptr && std::get_if<ir::Null>(&null_value->literal) != nullptr,
          "null is a null literal");
}

void test_unary_operators() {
    for (const std::string_view op : {"!", "-", "+"}) {
        const std::string source = std::string(op) + "a";
        const Lowered lowered = lower(source, &kNames);
        const ir::Expr* expr = expr_of(lowered);
        check(expr != nullptr && expr->kind == ir::ExprKind::Unary, source + " lowers to unary");
        if (expr == nullptr) {
            continue;
        }
        check_eq(expr->op, std::string(op), source + " keeps the operator");
        check(expr->operands.size() == 1, source + " has one operand");
        check(expr->operands.size() == 1 && expr->operands[0] &&
                  expr->operands[0]->kind == ir::ExprKind::Ref,
              source + " lowers its operand");
    }
}

void test_binary_operators() {
    // The IR operator set of ir-v1.md section 4.
    for (const std::string_view op : {"+", "-", "*", "/", "%", "<", "<=", ">", ">=", "&&", "||"}) {
        const std::string source = "a " + std::string(op) + " b";
        const Lowered lowered = lower(source, &kNames);
        const ir::Expr* expr = expr_of(lowered);
        check(expr != nullptr && expr->kind == ir::ExprKind::Binary, source + " lowers");
        if (expr != nullptr) {
            check_eq(expr->op, std::string(op), source + " keeps its operator");
            check(expr->operands.size() == 2, source + " has two operands");
        }
    }

    // `===`/`!==` map to `==`/`!=`: the IR has no implicit conversions.
    const Lowered equality_lowered = lower("a === b", &kNames);
    const ir::Expr* equality = expr_of(equality_lowered);
    check(equality != nullptr && equality->op == "==", "=== lowers to ==");
    const Lowered inequality_lowered = lower("a !== b", &kNames);
    const ir::Expr* inequality = expr_of(inequality_lowered);
    check(inequality != nullptr && inequality->op == "!=", "!== lowers to !=");
}

void test_binary_shape_follows_precedence() {
    const Lowered sum_lowered = lower("1 + 2 * 3");
    const ir::Expr* expression = expr_of(sum_lowered);
    check(expression != nullptr && expression->op == "+", "1 + 2 * 3 is a `+` at the top");
    if (expression != nullptr && expression->operands.size() == 2) {
        const ir::Expr* right = expression->operands[1].get();
        check(right != nullptr && right->kind == ir::ExprKind::Binary && right->op == "*",
              "the right operand is the `*` subtree");
    }

    const Lowered logical_lowered = lower("a && b || c", &kNames);
    const ir::Expr* logical = expr_of(logical_lowered);
    check(logical != nullptr && logical->op == "||", "&& binds tighter than ||");
    if (logical != nullptr && logical->operands.size() == 2 && logical->operands[0]) {
        check(logical->operands[0]->op == "&&", "the left operand of || is the && subtree");
    }
}

void test_conditional() {
    const Lowered lowered = lower("c ? a : b", &kNames);
    const ir::Expr* expr = expr_of(lowered);
    check(expr != nullptr && expr->kind == ir::ExprKind::Conditional, "a conditional lowers");
    if (expr == nullptr) {
        return;
    }
    check(expr->operands.size() == 3, "a conditional has three operands");
    if (expr->operands.size() == 3) {
        check(expr->operands[0] && expr->operands[0]->kind == ir::ExprKind::Ref &&
                  expr->operands[0]->id == "state::c",
              "the test is operand 0");
        check(expr->operands[1] && expr->operands[1]->id == "state::a", "then is operand 1");
        check(expr->operands[2] && expr->operands[2]->id == "state::b", "else is operand 2");
    }
}

void test_template_parts_interleave() {
    const Lowered lowered = lower("`n=${count}!`", &kNames);
    const ir::Expr* expr = expr_of(lowered);
    check(expr != nullptr && expr->kind == ir::ExprKind::Template, "a template lowers");
    if (expr == nullptr) {
        return;
    }
    check(expr->parts.size() == 3, "text, substitution, text");
    if (expr->parts.size() != 3) {
        return;
    }
    check(!expr->parts[0].expr && expr->parts[0].text == "n=", "the first part is text");
    check(expr->parts[1].expr && expr->parts[1].expr->id == "state::count",
          "the middle part is the substitution");
    check(!expr->parts[2].expr && expr->parts[2].text == "!", "the last part is text");

    const Lowered adjacent_lowered = lower("`${a}${b}`", &kNames);
    const ir::Expr* adjacent = expr_of(adjacent_lowered);
    check(adjacent != nullptr && adjacent->parts.size() == 5, "two substitutions give five parts");
    if (adjacent != nullptr && adjacent->parts.size() == 5) {
        check(!adjacent->parts[0].expr && adjacent->parts[0].text.empty(),
              "the empty leading text is kept");
        check(!adjacent->parts[4].expr && adjacent->parts[4].text.empty(),
              "the empty trailing text is kept");
    }
}

void test_calls_use_the_allowlist() {
    const Lowered min_lowered = lower("min(a, b)", &kNames);
    const ir::Expr* call = expr_of(min_lowered);
    check(call != nullptr && call->kind == ir::ExprKind::Call, "min lowers to a call");
    if (call != nullptr) {
        check_eq(call->id, "min", "the callee keeps its bare name");
        check(call->operands.size() == 2, "both arguments are kept");
        check(call->operands.size() == 2 && call->operands[0] &&
                  call->operands[0]->kind == ir::ExprKind::Ref,
              "arguments are lowered");
    }

    const Lowered nested_lowered = lower("max(min(a, b), 0)", &kNames);
    const ir::Expr* nested = expr_of(nested_lowered);
    check(nested != nullptr && nested->kind == ir::ExprKind::Call && nested->id == "max",
          "calls nest");
    check(nested != nullptr && !nested->operands.empty() && nested->operands[0] &&
              nested->operands[0]->kind == ir::ExprKind::Call,
          "the nested call is an argument");

    const Lowered single_lowered = lower("length(s)", &kNames);
    const ir::Expr* single = expr_of(single_lowered);
    check(single != nullptr && single->operands.size() == 1, "a call keeps its single argument");
}

void test_references_use_the_resolver_chain() {
    const Lowered name_lowered = lower("count", &kNames);
    const ir::Expr* name = expr_of(name_lowered);
    check(name != nullptr && name->kind == ir::ExprKind::Ref && name->id == "state::count",
          "an identifier becomes the resolver id");

    const Lowered prop_lowered = lower("props.title", &kNames);
    const ir::Expr* prop = expr_of(prop_lowered);
    check(prop != nullptr && prop->kind == ir::ExprKind::Ref && prop->id == "prop::title",
          "a props chain becomes a dotted prop id");

    const Lowered loop_lowered = lower("item.row.id", &kNames);
    const ir::Expr* loop = expr_of(loop_lowered);
    check(loop != nullptr && loop->kind == ir::ExprKind::Ref && loop->id == "item.row.id",
          "a loop item chain becomes a dotted local id");
}

void test_serialised_shapes() {
    const Lowered lowered = lower("a + b", &kNames);
    const ir::Expr* sum = expr_of(lowered);
    check(sum != nullptr, "a + b lowers");
    if (sum != nullptr) {
        check_eq(ir_text(*sum),
                 "{\n"
                 "  \"kind\": \"binary\",\n"
                 "  \"left\": {\n"
                 "    \"id\": \"state::a\",\n"
                 "    \"kind\": \"ref\"\n"
                 "  },\n"
                 "  \"op\": \"+\",\n"
                 "  \"right\": {\n"
                 "    \"id\": \"state::b\",\n"
                 "    \"kind\": \"ref\"\n"
                 "  }\n"
                 "}\n",
                 "binary serialises with left, op and right");
    }
}

} // namespace

int main() {
    test_integer_and_float_literals();
    test_string_boolean_and_null_literals();
    test_unary_operators();
    test_binary_operators();
    test_binary_shape_follows_precedence();
    test_conditional();
    test_template_parts_interleave();
    test_calls_use_the_allowlist();
    test_references_use_the_resolver_chain();
    test_serialised_shapes();
    return finish("lowering expression tests");
}
