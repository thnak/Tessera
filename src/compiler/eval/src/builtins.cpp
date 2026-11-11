// SPDX-License-Identifier: MIT

// The allowlisted pure functions of docs/specs/ir-v1.md section 4.2. They are total: a call never
// throws, and a bad argument type is a TESSERA_IR_EVAL_TYPE error, not a crash.

#include "operators.hpp"
#include "tessera/compiler/eval/number_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace tessera::compiler::eval::detail {
namespace {

constexpr std::int32_t kIntMin = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kIntMax = std::numeric_limits<std::int32_t>::max();

bool all_numbers(std::span<const Value> args) {
    return std::ranges::all_of(args, [](const Value& v) { return v.is_number(); });
}

std::unexpected<EvalError> arity(std::string_view name, std::size_t expected) {
    return fail("TESSERA_IR_EVAL_ARITY",
                "`" + std::string(name) + "` takes " + std::to_string(expected) + " argument(s)");
}

std::unexpected<EvalError> needs(std::string_view name, std::string_view what) {
    return fail("TESSERA_IR_EVAL_TYPE", "`" + std::string(name) + "` needs " + std::string(what));
}

// With a NaN operand the other one wins; both NaN stays NaN.
double pick(double a, double b, bool want_min) {
    if (std::isnan(a)) {
        return b;
    }
    if (std::isnan(b)) {
        return a;
    }
    return want_min ? std::min(a, b) : std::max(a, b);
}

Value select(const Value& a, const Value& b, bool want_min) {
    if (a.as_int() != nullptr && b.as_int() != nullptr) {
        return Value::of_int(want_min ? std::min(*a.as_int(), *b.as_int())
                                      : std::max(*a.as_int(), *b.as_int()));
    }
    return Value::of_float(pick(a.number(), b.number(), want_min));
}

std::int32_t to_int(double value) {
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= static_cast<double>(kIntMax)) {
        return kIntMax;
    }
    if (value <= static_cast<double>(kIntMin)) {
        return kIntMin;
    }
    return static_cast<std::int32_t>(std::trunc(value));
}

std::int32_t code_points(const std::string& text) {
    std::int64_t count = 0;
    for (const char c : text) {
        count += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return static_cast<std::int32_t>(std::min<std::int64_t>(count, kIntMax));
}

std::expected<Value, EvalError> rounding(std::string_view name, const Value& value) {
    if (!value.is_number()) {
        return needs(name, "a number");
    }
    const double x = value.number();
    if (name == "floor") {
        return Value::of_float(std::floor(x));
    }
    if (name == "ceil") {
        return Value::of_float(std::ceil(x));
    }
    return Value::of_float(std::round(x)); // ties away from zero
}

} // namespace

std::expected<Value, EvalError> call_function(std::string_view name, std::span<const Value> args) {
    if (name == "min" || name == "max") {
        if (args.size() != 2) {
            return arity(name, 2);
        }
        if (!all_numbers(args)) {
            return needs(name, "two numbers");
        }
        return select(args[0], args[1], name == "min");
    }
    if (name == "clamp") {
        if (args.size() != 3) {
            return arity(name, 3);
        }
        if (!all_numbers(args)) {
            return needs(name, "three numbers");
        }
        // min(max(x, lo), hi), typed as the arguments are: all int stays int.
        return select(select(args[0], args[1], false), args[2], true);
    }
    if (name == "abs") {
        if (args.size() != 1) {
            return arity(name, 1);
        }
        if (const std::int32_t* integer = args[0].as_int()) {
            return Value::of_int(*integer == kIntMin ? kIntMin : std::abs(*integer));
        }
        if (const double* number = args[0].as_float()) {
            return Value::of_float(std::fabs(*number));
        }
        return needs(name, "a number");
    }
    if (name == "floor" || name == "ceil" || name == "round") {
        return args.size() == 1 ? rounding(name, args[0]) : arity(name, 1);
    }
    if (name == "toInt") {
        if (args.size() != 1) {
            return arity(name, 1);
        }
        if (const std::int32_t* integer = args[0].as_int()) {
            return Value::of_int(*integer);
        }
        if (const double* number = args[0].as_float()) {
            return Value::of_int(to_int(*number));
        }
        return needs(name, "a number");
    }
    if (name == "toFloat") {
        if (args.size() != 1) {
            return arity(name, 1);
        }
        if (!args[0].is_number()) {
            return needs(name, "a number");
        }
        return Value::of_float(args[0].number());
    }
    if (name == "length") {
        if (args.size() != 1) {
            return arity(name, 1);
        }
        if (const std::string* text = args[0].as_string()) {
            return Value::of_int(code_points(*text));
        }
        if (const List* list = args[0].as_list()) {
            return Value::of_int(
                static_cast<std::int32_t>(std::min<std::size_t>(list->size(), kIntMax)));
        }
        return needs(name, "a string or a list");
    }
    if (name == "formatNumber") {
        if (args.size() != 2) {
            return arity(name, 2);
        }
        if (!args[0].is_number() || args[1].as_int() == nullptr) {
            return needs(name, "a number and an int count of decimals");
        }
        return Value::of_string(fixed_text(args[0].number(), *args[1].as_int()));
    }
    return fail("TESSERA_IR_EVAL_FUNCTION",
                "`" + std::string(name) + "` is not an allowlisted function");
}

} // namespace tessera::compiler::eval::detail
