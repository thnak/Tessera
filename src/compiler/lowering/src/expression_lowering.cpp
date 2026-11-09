// SPDX-License-Identifier: MIT

// Entry point and operator dispatch for expression lowering (docs/design/jsx-ir-lowering.md
// section 9, slice 5; docs/specs/jsx-subset-v1.md section 8.2).

#include "expression_lowering_internal.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kUnsupported = "TESSERA_IR_UNSUPPORTED";
constexpr std::string_view kMalformed = "TESSERA_IR_MALFORMED";

bool is_ir_binary_op(std::string_view op) {
    constexpr std::string_view kOps[] = {"+", "-", "*", "/", "%", "<", "<=", ">", ">=", "&&", "||"};
    for (const std::string_view candidate : kOps) {
        if (candidate == op) {
            return true;
        }
    }
    return false;
}

bool is_ir_unary_op(std::string_view op) {
    return op == "!" || op == "-" || op == "+";
}

// `===` and `!==` are the only equality the parser lets through (loose `==`/`!=` is rejected at
// parse time, docs/specs/jsx-subset-v1.md section 8.2); the IR has no implicit conversions, so
// they become `==`/`!=`. A hand-built AST with a loose operator is rejected instead of mapped.
bool map_binary_op(std::string_view source, std::string& ir_op) {
    if (source == "===") {
        ir_op = "==";
        return true;
    }
    if (source == "!==") {
        ir_op = "!=";
        return true;
    }
    if (is_ir_binary_op(source)) {
        ir_op = std::string(source);
        return true;
    }
    return false;
}

bool check_operands(LoweringState& state, const jsx::Expr& expr, std::size_t count,
                    std::string_view what) {
    if (expr.operands.size() != count) {
        fail(state, kMalformed,
             std::string(what) + " needs " + std::to_string(count) + " operand(s)", expr.span);
        return false;
    }
    for (const jsx::ExprPtr& operand : expr.operands) {
        if (!operand) {
            fail(state, kMalformed, std::string(what) + " has a missing operand", expr.span);
            return false;
        }
    }
    return true;
}

ir::ExprPtr lower_conditional(LoweringState& state, const jsx::Expr& expr) {
    if (!check_operands(state, expr, 3, "a conditional")) {
        return nullptr;
    }
    ir::ExprPtr test = lower_expr(state, *expr.operands[0]);
    if (!test) {
        return nullptr;
    }
    ir::ExprPtr then_value = lower_expr(state, *expr.operands[1]);
    if (!then_value) {
        return nullptr;
    }
    ir::ExprPtr else_value = lower_expr(state, *expr.operands[2]);
    if (!else_value) {
        return nullptr;
    }
    return ir::make_conditional(std::move(test), std::move(then_value), std::move(else_value));
}

ir::ExprPtr lower_unary(LoweringState& state, const jsx::Expr& expr) {
    if (!check_operands(state, expr, 1, "a unary expression")) {
        return nullptr;
    }
    if (!is_ir_unary_op(expr.text)) {
        fail(state, kUnsupported,
             "unsupported unary operator `" + expr.text +
                 "`; ir-v1.md section 4 allows `!`, `-` and `+`",
             expr.span);
        return nullptr;
    }
    ir::ExprPtr value = lower_expr(state, *expr.operands[0]);
    if (!value) {
        return nullptr;
    }
    return ir::make_unary(expr.text, std::move(value));
}

ir::ExprPtr lower_binary(LoweringState& state, const jsx::Expr& expr) {
    if (!check_operands(state, expr, 2, "a binary expression")) {
        return nullptr;
    }
    std::string op;
    if (!map_binary_op(expr.text, op)) {
        const std::string suggestion = expr.text == "==" || expr.text == "!="
                                           ? "use `===` or `!==`"
                                           : "ir-v1.md section 4 lists the supported operators";
        fail(state, kUnsupported, "unsupported binary operator `" + expr.text + "`", expr.span,
             suggestion);
        return nullptr;
    }
    ir::ExprPtr left = lower_expr(state, *expr.operands[0]);
    if (!left) {
        return nullptr;
    }
    ir::ExprPtr right = lower_expr(state, *expr.operands[1]);
    if (!right) {
        return nullptr;
    }
    return ir::make_binary(op, std::move(left), std::move(right));
}

} // namespace

ir::ExprPtr lower_expr(LoweringState& state, const jsx::Expr& expr) {
    switch (expr.kind) {
    case jsx::ExprKind::Number:
    case jsx::ExprKind::String:
    case jsx::ExprKind::Boolean:
    case jsx::ExprKind::Null:
        return lower_literal(state, expr);
    case jsx::ExprKind::Identifier:
    case jsx::ExprKind::Member:
        return lower_reference(state, expr);
    case jsx::ExprKind::Unary:
        return lower_unary(state, expr);
    case jsx::ExprKind::Binary:
        return lower_binary(state, expr);
    case jsx::ExprKind::Conditional:
        return lower_conditional(state, expr);
    case jsx::ExprKind::Template:
        return lower_template(state, expr);
    case jsx::ExprKind::Call:
        return lower_call(state, expr);
    case jsx::ExprKind::Array:
        fail(state, kUnsupported,
             "an array literal is not an IR expression; V1 accepts one only as a list source or "
             "an initial value (jsx-subset-v1.md sections 8.4 and 10.5)",
             expr.span);
        return nullptr;
    case jsx::ExprKind::Object:
        fail(state, kUnsupported,
             "an object literal is not an IR expression; V1 accepts one only as a `style` or "
             "`props` value (jsx-subset-v1.md sections 8.5 and 10.3)",
             expr.span);
        return nullptr;
    case jsx::ExprKind::Arrow:
        fail(state, kUnsupported,
             "a function value is not an IR expression; handlers are lowered separately "
             "(slice 6)",
             expr.span);
        return nullptr;
    case jsx::ExprKind::Jsx:
        fail(state, kUnsupported,
             "JSX inside an expression is lowered with the node rules, not the expression rules "
             "(slice 7)",
             expr.span);
        return nullptr;
    case jsx::ExprKind::Return:
        fail(state, kMalformed, "`return` is only valid inside an arrow body", expr.span);
        return nullptr;
    }
    fail(state, kUnsupported, "the expression kind has no IR form", expr.span);
    return nullptr;
}

} // namespace tessera::compiler::lowering::internal

namespace tessera::compiler::lowering {

ExpressionLoweringResult lower_expression(const jsx::Expr& expr,
                                          const ExpressionLoweringContext& context) {
    internal::LoweringState state{context, std::nullopt};
    ir::ExprPtr lowered = internal::lower_expr(state, expr);
    if (!lowered && !state.error) {
        // Defensive: every branch above reports before returning null.
        internal::fail(state, "TESSERA_IR_MALFORMED", "the expression produced no IR", expr.span);
    }
    if (state.error) {
        ExpressionLoweringErrors errors;
        errors.push_back(std::move(*state.error));
        return std::unexpected(std::move(errors));
    }
    return lowered;
}

} // namespace tessera::compiler::lowering
