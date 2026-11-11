// SPDX-License-Identifier: MIT

// Operators of the IR expression language with the numeric rules of docs/specs/ir-v1.md
// section 4.1.

#include "operators.hpp"
#include "tessera/compiler/eval/number_text.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace tessera::compiler::eval::detail {
namespace {

constexpr std::int32_t kIntMin = std::numeric_limits<std::int32_t>::min();

// `int` arithmetic wraps modulo 2^32; generated C++ does it in uint32_t so there is no undefined
// behavior, and so does this.
std::int32_t wrap(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}
std::uint32_t bits(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}

template<typename T> bool compare(std::string_view op, T left, T right) {
    if (op == "<") {
        return left < right;
    }
    if (op == "<=") {
        return left <= right;
    }
    if (op == ">") {
        return left > right;
    }
    return left >= right;
}

bool is_ordering(std::string_view op) {
    return op == "<" || op == "<=" || op == ">" || op == ">=";
}

std::expected<Value, EvalError> equality(std::string_view op, const Value& left,
                                         const Value& right) {
    bool equal = false;
    if (left.is_number() && right.is_number()) {
        const std::int32_t* a = left.as_int();
        const std::int32_t* b = right.as_int();
        equal = (a != nullptr && b != nullptr) ? *a == *b : left.number() == right.number();
    } else if (left.as_bool() != nullptr && right.as_bool() != nullptr) {
        equal = *left.as_bool() == *right.as_bool();
    } else if (left.as_string() != nullptr && right.as_string() != nullptr) {
        equal = *left.as_string() == *right.as_string();
    } else if (left.is_null() && right.is_null()) {
        equal = true;
    } else {
        return fail("TESSERA_IR_EVAL_TYPE", "`" + std::string(op) + "` cannot compare " +
                                                std::string(type_name(left)) + " with " +
                                                std::string(type_name(right)));
    }
    return Value::of_bool(op == "==" ? equal : !equal);
}

std::expected<Value, EvalError> integer_arithmetic(std::string_view op, std::int32_t a,
                                                   std::int32_t b, bool& modulo_zero) {
    if (op == "+") {
        return Value::of_int(wrap(bits(a) + bits(b)));
    }
    if (op == "-") {
        return Value::of_int(wrap(bits(a) - bits(b)));
    }
    if (op == "*") {
        return Value::of_int(wrap(bits(a) * bits(b)));
    }
    // `%`: the sign of the left operand; x % 0 is 0 with a diagnostic; INT_MIN % -1 is 0.
    if (b == 0) {
        modulo_zero = true;
        return Value::of_int(0);
    }
    if (a == kIntMin && b == -1) {
        return Value::of_int(0);
    }
    return Value::of_int(a % b);
}

std::expected<Value, EvalError> float_arithmetic(std::string_view op, double a, double b) {
    if (op == "+") {
        return Value::of_float(a + b);
    }
    if (op == "-") {
        return Value::of_float(a - b);
    }
    if (op == "*") {
        return Value::of_float(a * b);
    }
    if (op == "/") {
        return Value::of_float(a / b);
    }
    return Value::of_float(std::fmod(a, b));
}

} // namespace

std::unexpected<EvalError> fail(std::string code, std::string message) {
    return std::unexpected(EvalError{std::move(code), std::move(message)});
}

std::expected<Value, EvalError> apply_binary(std::string_view op, const Value& left,
                                             const Value& right, bool& modulo_zero) {
    if (op == "==" || op == "!=") {
        return equality(op, left, right);
    }
    if (!left.is_number() || !right.is_number()) {
        return fail("TESSERA_IR_EVAL_TYPE", "`" + std::string(op) + "` needs two numbers, got " +
                                                std::string(type_name(left)) + " and " +
                                                std::string(type_name(right)));
    }
    const std::int32_t* a = left.as_int();
    const std::int32_t* b = right.as_int();
    const bool both_int = a != nullptr && b != nullptr;
    if (is_ordering(op)) {
        return Value::of_bool(both_int ? compare(op, *a, *b)
                                       : compare(op, left.number(), right.number()));
    }
    if (op == "+" || op == "-" || op == "*" || op == "%") {
        if (both_int) {
            return integer_arithmetic(op, *a, *b, modulo_zero);
        }
        return float_arithmetic(op, left.number(), right.number());
    }
    if (op == "/") {
        return float_arithmetic(op, left.number(), right.number());
    }
    return fail("TESSERA_IR_EVAL_OPERATOR", "unknown binary operator `" + std::string(op) + "`");
}

std::expected<Value, EvalError> apply_unary(std::string_view op, const Value& value) {
    if (op == "!") {
        if (const bool* flag = value.as_bool()) {
            return Value::of_bool(!*flag);
        }
        return fail("TESSERA_IR_EVAL_TYPE",
                    "`!` needs a bool, got " + std::string(type_name(value)));
    }
    if (op != "-" && op != "+") {
        return fail("TESSERA_IR_EVAL_OPERATOR", "unknown unary operator `" + std::string(op) + "`");
    }
    if (const std::int32_t* integer = value.as_int()) {
        // -INT_MIN wraps to INT_MIN.
        return Value::of_int(op == "-" ? wrap(0U - bits(*integer)) : *integer);
    }
    if (const double* number = value.as_float()) {
        return Value::of_float(op == "-" ? -*number : *number);
    }
    return fail("TESSERA_IR_EVAL_TYPE",
                "`" + std::string(op) + "` needs a number, got " + std::string(type_name(value)));
}

std::expected<std::string, EvalError> template_text(const Value& value) {
    if (const std::string* text = value.as_string()) {
        return *text;
    }
    if (const std::int32_t* integer = value.as_int()) {
        return int_text(*integer);
    }
    if (const double* number = value.as_float()) {
        return float_text(*number);
    }
    return fail("TESSERA_IR_EVAL_TYPE",
                "a " + std::string(type_name(value)) + " has no text form in a template");
}

} // namespace tessera::compiler::eval::detail
