// SPDX-License-Identifier: MIT

// The allowlisted pure functions of docs/specs/ir-v1.md section 4.2. They are total: a call never
// throws, and a bad argument type is a TESSERA_IR_EVAL_TYPE error, not a crash.

#include "operators.hpp"
#include "tessera/compiler/eval/number_text.hpp"

#include <algorithm>
#include <charconv>
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

// ADR-066 item 3: `trim` removes ASCII whitespace from both ends. It is deliberately ASCII-only, so
// the result does not depend on a locale or on the host's Unicode tables.
std::string trimmed(const std::string& text) {
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// Issue #803: the strict parse behind `parseIntOr`/`parseFloatOr`. Whole-string, not a leading
// scan: the input is `trim`med, then must be *entirely* a number, or the fallback wins. That is
// the deliberate difference from JavaScript's `parseInt`, whose leading scan accepts `12abc` â€”
// a form field holds exactly such text, and silently reading `12` out of it is the mistake the
// `Or` form exists to avoid.
//
// Accepted syntax: an optional `+`/`-` sign, decimal digits with at most one decimal point
// (`parseIntOr` rejects the point), and no exponent, no hex, no separators â€” `1e3` and `0x10`
// are unparseable text here, because a ported `Number("0x10")` reading 16 would be a silent
// reinterpretation. Out of range saturates to the type's limit rather than falling back: the
// string *is* a number, and the fallback is for the not-a-number case only.
[[nodiscard]] bool parse_number_strict(std::string_view text, bool integral, double& out) {
    // The trimmed text must own its storage: assigning a temporary string to the view would leave
    // it dangling, and the parse reads it afterwards.
    const std::string owned_trimmed = trimmed(std::string(text));
    const std::string_view body = owned_trimmed;
    if (body.empty()) {
        return false;
    }
    std::size_t at = 0;
    bool negative = false;
    if (body[at] == '+' || body[at] == '-') {
        // `from_chars` accepts a leading `-` but not `+` for integers (the standard's grammar), so
        // the sign is consumed here and re-attached to the value below.
        negative = body[at] == '-';
        ++at;
    }
    // The digits begin here, right after any sign: this is where the parse starts. (`at` keeps
    // walking below, so it is not the start once the loop has run.)
    const char* const digits_first = body.data() + at;
    std::size_t digits = 0;
    bool point = false;
    for (; at < body.size(); ++at) {
        const char c = body[at];
        if (c >= '0' && c <= '9') {
            ++digits;
            continue;
        }
        if (c == '.' && !point) {
            if (integral) {
                return false; // an integer literal has no decimal point
            }
            point = true;
            continue;
        }
        return false;
    }
    if (digits == 0) {
        return false; // a sign alone, or a bare point, is not a number
    }
    const char* first = digits_first;
    const char* last = body.data() + body.size();
    if (integral) {
        // `int64_t`, not `int32_t`: `from_chars` reports out of range rather than a partial read,
        // and the rule is to saturate, not to fall back. Parsing wide, applying the sign, and
        // clamping to the `int` range gives the saturation; a direct `int32_t` parse would report
        // out-of-range as an error and a naive narrowing cast would wrap.
        std::int64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(first, last, parsed, 10);
        if (ec != std::errc{} || ptr != last) {
            return false;
        }
        const std::int64_t signed_value = negative ? -parsed : parsed;
        out = static_cast<double>(std::clamp(signed_value, static_cast<std::int64_t>(kIntMin),
                                             static_cast<std::int64_t>(kIntMax)));
        return true;
    }
    double parsed = 0;
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last) {
        return false;
    }
    out = negative ? -parsed : parsed;
    return true;
}

// ADR-066 item 3: `toUpperCase`/`toLowerCase` fold ASCII letters only. A non-ASCII byte passes
// through unchanged, so the result is a pure function of the input bytes.
std::string cased(const std::string& text, bool upper) {
    std::string out = text;
    for (char& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 'a' && byte <= 'z' && upper) {
            c = static_cast<char>(byte - 'a' + 'A');
        } else if (byte >= 'A' && byte <= 'Z' && !upper) {
            c = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return out;
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
    // ADR-066 item 3: the total string methods. Each takes its receiver as the first argument, so a
    // method call and a function call are one shape.
    if (name == "trim" || name == "toUpperCase" || name == "toLowerCase") {
        if (args.size() != 1) {
            return arity(name, 1);
        }
        const std::string* text = args[0].as_string();
        if (text == nullptr) {
            return needs(name, "a string");
        }
        if (name == "trim") {
            return Value::of_string(trimmed(*text));
        }
        return Value::of_string(cased(*text, name == "toUpperCase"));
    }
    if (name == "startsWith") {
        if (args.size() != 2) {
            return arity(name, 2);
        }
        const std::string* text = args[0].as_string();
        const std::string* prefix = args[1].as_string();
        if (text == nullptr || prefix == nullptr) {
            return needs(name, "two strings");
        }
        return Value::of_bool(text->starts_with(*prefix));
    }
    if (name == "slice") {
        // The list form is `listSlice`; this is the string form, `s.slice(start[, end])`.
        if (args.size() < 2 || args.size() > 3) {
            return fail("TESSERA_IR_EVAL_ARITY", "`slice` takes a string and one or two bounds");
        }
        const std::string* text = args[0].as_string();
        if (text == nullptr) {
            return needs(name, "a string");
        }
        const auto bound = [&](std::size_t index,
                               std::int64_t fallback) -> std::expected<std::int64_t, EvalError> {
            if (index >= args.size()) {
                return fallback;
            }
            const std::int32_t* integer = args[index].as_int();
            if (integer == nullptr) {
                return fail("TESSERA_IR_EVAL_TYPE", "a `slice` bound must be an int");
            }
            return static_cast<std::int64_t>(*integer);
        };
        auto start = bound(1, 0);
        if (!start) {
            return std::unexpected(std::move(start.error()));
        }
        auto end = bound(2, static_cast<std::int64_t>(text->size()));
        if (!end) {
            return std::unexpected(std::move(end.error()));
        }
        const auto size = static_cast<std::int64_t>(text->size());
        const std::int64_t from = std::clamp<std::int64_t>(*start, 0, size);
        const std::int64_t to = std::clamp<std::int64_t>(*end, from, size);
        return Value::of_string(
            text->substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
    }
    if (name == "parseIntOr" || name == "parseFloatOr") {
        if (args.size() != 2) {
            return arity(name, 2);
        }
        const std::string* text = args[0].as_string();
        if (text == nullptr) {
            return needs(name, "a string and the fallback");
        }
        // The fallback type is checked, not coerced: `parseIntOr(s, 1.5)` would silently truncate
        // in the generated C++ too, so the evaluator rejects it here.
        const bool integral = name == "parseIntOr";
        if (integral ? args[1].as_int() == nullptr : !args[1].is_number()) {
            return needs(name, integral ? "an int fallback" : "a number fallback");
        }
        double parsed = 0;
        if (!parse_number_strict(*text, integral, parsed)) {
            return args[1];
        }
        return integral ? Value::of_int(static_cast<std::int32_t>(parsed))
                        : Value::of_float(parsed);
    }
    if (name == "toFixed") {
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
