// SPDX-License-Identifier: MIT

#include "operators.hpp"
#include "tessera/compiler/eval/evaluator.hpp"

#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

namespace tessera::compiler::eval {
namespace {

using detail::fail;

constexpr std::string_view kState = "state::";
constexpr std::string_view kAnim = "anim::";

std::expected<Value, EvalError> literal_value(const ir::LiteralValue& literal) {
    if (const bool* flag = std::get_if<bool>(&literal)) {
        return Value::of_bool(*flag);
    }
    if (const std::int64_t* integer = std::get_if<std::int64_t>(&literal)) {
        if (*integer < std::numeric_limits<std::int32_t>::min() ||
            *integer > std::numeric_limits<std::int32_t>::max()) {
            return fail("TESSERA_IR_EVAL_LITERAL_RANGE",
                        "the int literal " + std::to_string(*integer) + " does not fit 32 bits");
        }
        return Value::of_int(static_cast<std::int32_t>(*integer));
    }
    if (const double* number = std::get_if<double>(&literal)) {
        return Value::of_float(*number);
    }
    if (const std::string* text = std::get_if<std::string>(&literal)) {
        return Value::of_string(*text);
    }
    return Value::null();
}

bool has_operands(const ir::Expr& expr, std::size_t count) {
    if (expr.operands.size() != count) {
        return false;
    }
    for (const ir::ExprPtr& operand : expr.operands) {
        if (!operand) {
            return false;
        }
    }
    return true;
}

std::unexpected<EvalError> malformed(std::string_view what) {
    return fail("TESSERA_IR_EVAL_MALFORMED", std::string(what));
}

} // namespace

std::expected<Value, EvalError> Evaluator::evaluate(const ir::Expr& expr, const Scope& scope) {
    return eval(expr, scope, 0);
}

void Evaluator::report_modulo_zero(const ir::Expr& expr) {
    if (reported_.insert(&expr).second) {
        diagnostics_.push_back(
            {"TESSERA_IR_INT_MODULO_ZERO", "`%` with a zero int divisor; the result is 0"});
    }
}

std::expected<Value, EvalError> Evaluator::eval(const ir::Expr& expr, const Scope& scope,
                                                std::size_t depth) {
    if (depth > kMaxDepth) {
        return fail("TESSERA_IR_EVAL_DEPTH",
                    "the expression nests deeper than the evaluator allows");
    }
    switch (expr.kind) {
    case ir::ExprKind::Literal:
        return literal_value(expr.literal);
    case ir::ExprKind::Ref: {
        const auto found = scope.find(expr.id);
        if (found == scope.end()) {
            return fail("TESSERA_IR_EVAL_UNKNOWN_REF", "`" + expr.id + "` is not in scope");
        }
        return found->second;
    }
    case ir::ExprKind::Binary:
        return eval_binary(expr, scope, depth);
    case ir::ExprKind::Unary: {
        if (!has_operands(expr, 1)) {
            return malformed("a unary expression needs one operand");
        }
        auto value = eval(*expr.operands[0], scope, depth + 1);
        if (!value) {
            return value;
        }
        return detail::apply_unary(expr.op, *value);
    }
    case ir::ExprKind::Conditional: {
        if (!has_operands(expr, 3)) {
            return malformed("a conditional needs three operands");
        }
        auto test = eval(*expr.operands[0], scope, depth + 1);
        if (!test) {
            return test;
        }
        const bool* flag = test->as_bool();
        if (flag == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "a conditional test must be a bool, got " + std::string(type_name(*test)));
        }
        // Only the chosen branch runs, so the other cannot report a diagnostic or an error.
        return eval(*expr.operands[*flag ? 1 : 2], scope, depth + 1);
    }
    case ir::ExprKind::Template:
        return eval_template(expr, scope, depth);
    case ir::ExprKind::Call:
        return eval_call(expr, scope, depth);
    case ir::ExprKind::Array: {
        List items;
        items.reserve(expr.operands.size());
        for (const ir::ExprPtr& operand : expr.operands) {
            if (!operand) {
                return malformed("an array element is missing");
            }
            auto item = eval(*operand, scope, depth + 1);
            if (!item) {
                return item;
            }
            items.push_back(std::move(*item));
        }
        return Value::of_list(std::move(items));
    }
    }
    return malformed("unknown expression kind");
}

std::expected<Value, EvalError> Evaluator::eval_binary(const ir::Expr& expr, const Scope& scope,
                                                       std::size_t depth) {
    if (!has_operands(expr, 2)) {
        return malformed("a binary expression needs two operands");
    }
    auto left = eval(*expr.operands[0], scope, depth + 1);
    if (!left) {
        return left;
    }
    if (expr.op == "&&" || expr.op == "||") {
        const bool* lhs = left->as_bool();
        if (lhs == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "`" + expr.op + "` needs bools, got " + std::string(type_name(*left)));
        }
        const bool decided = expr.op == "&&" ? !*lhs : *lhs;
        if (decided) {
            return Value::of_bool(*lhs); // short circuit: the right side never runs
        }
        auto right = eval(*expr.operands[1], scope, depth + 1);
        if (!right) {
            return right;
        }
        if (right->as_bool() == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "`" + expr.op + "` needs bools, got " + std::string(type_name(*right)));
        }
        return Value::of_bool(*right->as_bool());
    }
    auto right = eval(*expr.operands[1], scope, depth + 1);
    if (!right) {
        return right;
    }
    bool modulo_zero = false;
    auto result = detail::apply_binary(expr.op, *left, *right, modulo_zero);
    if (modulo_zero) {
        report_modulo_zero(expr);
    }
    return result;
}

std::expected<Value, EvalError> Evaluator::eval_template(const ir::Expr& expr, const Scope& scope,
                                                         std::size_t depth) {
    std::string text;
    for (const ir::TemplatePart& part : expr.parts) {
        if (!part.expr) {
            text += part.text;
            continue;
        }
        auto value = eval(*part.expr, scope, depth + 1);
        if (!value) {
            return value;
        }
        auto piece = detail::template_text(*value);
        if (!piece) {
            return std::unexpected(std::move(piece.error()));
        }
        text += *piece;
    }
    return Value::of_string(std::move(text));
}

std::expected<Value, EvalError> Evaluator::eval_call(const ir::Expr& expr, const Scope& scope,
                                                     std::size_t depth) {
    std::vector<Value> args;
    args.reserve(expr.operands.size());
    for (const ir::ExprPtr& operand : expr.operands) {
        if (!operand) {
            return malformed("a call argument is missing");
        }
        auto value = eval(*operand, scope, depth + 1);
        if (!value) {
            return value;
        }
        args.push_back(std::move(*value));
    }
    return detail::call_function(expr.id, args);
}

std::expected<void, EvalError> Evaluator::run(std::span<const ir::Action> actions, Scope& scope) {
    for (const ir::Action& action : actions) {
        if (!action.value) {
            return fail("TESSERA_IR_EVAL_MALFORMED", "an action has no value expression");
        }
        const bool state_target =
            action.kind == ir::ActionKind::Set && action.target.rfind(kState, 0) == 0;
        const bool anim_target =
            action.kind == ir::ActionKind::AnimSet && action.target.rfind(kAnim, 0) == 0;
        if (!state_target && !anim_target) {
            return fail("TESSERA_IR_EVAL_ACTION_TARGET",
                        "`" + action.target + "` is not a valid target for this action");
        }
        const auto target = scope.find(action.target);
        if (target == scope.end()) {
            return fail("TESSERA_IR_EVAL_UNKNOWN_REF",
                        "`" + action.target + "` is not declared in scope");
        }
        auto value = eval(*action.value, scope, 0);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        // The target keeps its type; an int widens into a float as C++ converts it.
        if (target->second.as_float() != nullptr && value->as_int() != nullptr) {
            *value = Value::of_float(value->number());
        }
        if (target->second.data.index() != value->data.index()) {
            return fail("TESSERA_IR_EVAL_TYPE", "`" + action.target + "` holds a " +
                                                    std::string(type_name(target->second)) +
                                                    ", cannot assign a " +
                                                    std::string(type_name(*value)));
        }
        target->second = std::move(*value);
    }
    return {};
}

} // namespace tessera::compiler::eval
