// SPDX-License-Identifier: MIT
//
// The IR evaluator (docs/specs/ir-v1.md sections 4.1 and 4.2): every row of the numeric table,
// each pure function, template text, short-circuit operators, the modulo-by-zero diagnostic and the
// action semantics. These are the values generated C++ must reproduce.

#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/eval/number_text.hpp"
#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/expr.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ir = tessera::compiler::ir;
namespace eval = tessera::compiler::eval;

constexpr std::int32_t kIntMin = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kIntMax = std::numeric_limits<std::int32_t>::max();
const double kNan = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ir_eval: " << message << "\n";
    }
    return condition;
}

ir::ExprPtr lit(std::int64_t value) {
    return ir::make_literal(value);
}
ir::ExprPtr lit(int value) {
    return ir::make_literal(std::int64_t{value});
}
ir::ExprPtr lit(double value) {
    return ir::make_literal(value);
}
ir::ExprPtr lit(bool value) {
    return ir::make_literal(value);
}
ir::ExprPtr text(const std::string& value) {
    return ir::make_literal(value);
}
ir::ExprPtr bin(const std::string& op, ir::ExprPtr a, ir::ExprPtr b) {
    return ir::make_binary(op, std::move(a), std::move(b));
}
ir::ExprPtr call(const std::string& name, ir::ExprPtr a) {
    std::vector<ir::ExprPtr> args;
    args.push_back(std::move(a));
    return ir::make_call(name, std::move(args));
}
ir::ExprPtr call(const std::string& name, ir::ExprPtr a, ir::ExprPtr b) {
    std::vector<ir::ExprPtr> args;
    args.push_back(std::move(a));
    args.push_back(std::move(b));
    return ir::make_call(name, std::move(args));
}
ir::ExprPtr call(const std::string& name, ir::ExprPtr a, ir::ExprPtr b, ir::ExprPtr c) {
    std::vector<ir::ExprPtr> args;
    args.push_back(std::move(a));
    args.push_back(std::move(b));
    args.push_back(std::move(c));
    return ir::make_call(name, std::move(args));
}

std::expected<eval::Value, eval::EvalError> run(const ir::ExprPtr& expr, eval::Evaluator& ev,
                                                const eval::Scope& scope = {}) {
    return ev.evaluate(*expr, scope);
}

bool is_int(const ir::ExprPtr& expr, std::int32_t want, const std::string& what) {
    eval::Evaluator ev;
    const auto value = run(expr, ev);
    return expect(value && value->as_int() != nullptr && *value->as_int() == want, what);
}

bool is_float(const ir::ExprPtr& expr, double want, const std::string& what) {
    eval::Evaluator ev;
    const auto value = run(expr, ev);
    if (!value || value->as_float() == nullptr) {
        return expect(false, what + " (not a float)");
    }
    const double got = *value->as_float();
    const bool same = std::isnan(want) ? std::isnan(got)
                                       : (got == want && std::signbit(got) == std::signbit(want));
    return expect(same, what + " (got " + eval::float_text(got) + ")");
}

bool is_bool(const ir::ExprPtr& expr, bool want, const std::string& what) {
    eval::Evaluator ev;
    const auto value = run(expr, ev);
    return expect(value && value->as_bool() != nullptr && *value->as_bool() == want, what);
}

bool is_text(const ir::ExprPtr& expr, const std::string& want, const std::string& what) {
    eval::Evaluator ev;
    const auto value = run(expr, ev);
    return expect(value && value->as_string() != nullptr && *value->as_string() == want,
                  what + " (got " + (value ? eval::describe(*value) : value.error().code) + ")");
}

bool fails(const ir::ExprPtr& expr, const std::string& code, const std::string& what) {
    eval::Evaluator ev;
    const auto value = run(expr, ev);
    return expect(!value && value.error().code == code, what);
}

bool integers() {
    bool ok = is_int(bin("+", lit(2), lit(3)), 5, "int addition");
    ok = is_int(bin("+", lit(kIntMax), lit(1)), kIntMin, "int + overflow wraps") && ok;
    ok = is_int(bin("-", lit(kIntMin), lit(1)), kIntMax, "int - overflow wraps") && ok;
    ok = is_int(bin("*", lit(kIntMax), lit(2)), -2, "int * overflow wraps") && ok;
    ok = is_int(bin("*", lit(65536), lit(65536)), 0, "int * wraps to zero") && ok;
    ok =
        is_int(ir::make_unary("-", lit(kIntMin)), kIntMin, "- of the minimum int is the minimum") &&
        ok;
    ok = is_int(ir::make_unary("-", lit(5)), -5, "unary minus") && ok;
    ok = is_int(ir::make_unary("+", lit(5)), 5, "unary plus") && ok;
    // `%`: the sign of the left operand; zero divisor is 0; INT_MIN % -1 is 0.
    ok = is_int(bin("%", lit(7), lit(3)), 1, "7 % 3") && ok;
    ok = is_int(bin("%", lit(-7), lit(3)), -1, "-7 % 3 has the sign of the left operand") && ok;
    ok = is_int(bin("%", lit(7), lit(-3)), 1, "7 % -3") && ok;
    ok = is_int(bin("%", lit(kIntMin), lit(-1)), 0, "INT_MIN % -1 is 0") && ok;
    ok = is_int(bin("%", lit(5), lit(0)), 0, "x % 0 is 0") && ok;
    return ok;
}

bool floats() {
    bool ok = is_float(bin("/", lit(7), lit(2)), 3.5, "int / int is a float");
    ok = is_float(bin("/", lit(1), lit(0)), kInf, "1 / 0 is Infinity") && ok;
    ok = is_float(bin("/", lit(-1), lit(0)), -kInf, "-1 / 0 is -Infinity") && ok;
    ok = is_float(bin("/", lit(0), lit(0)), kNan, "0 / 0 is NaN") && ok;
    ok = is_float(bin("+", lit(1), lit(0.5)), 1.5, "int + float converts the int") && ok;
    ok = is_float(bin("*", lit(0.1), lit(3)), 0.1 * 3.0, "float arithmetic is binary64") && ok;
    ok = is_float(bin("%", lit(7.5), lit(2)), 1.5, "float % is fmod") && ok;
    ok = is_float(bin("%", lit(-7.5), lit(2)), -1.5, "fmod keeps the sign of the left operand") &&
         ok;
    ok = is_float(bin("%", lit(1.0), lit(0.0)), kNan, "float % 0 is NaN") && ok;
    ok = is_float(ir::make_unary("-", lit(0.0)), -0.0, "-0.0 keeps its sign") && ok;
    return ok;
}

bool comparisons() {
    bool ok = is_bool(bin("<", lit(1), lit(2)), true, "1 < 2");
    ok = is_bool(bin(">=", lit(2), lit(2)), true, "2 >= 2") && ok;
    ok = is_bool(bin("==", lit(2), lit(2.0)), true, "an int equals the same float") && ok;
    ok = is_bool(bin("<", lit(1), lit(1.5)), true, "mixed comparison converts the int") && ok;
    ok = is_bool(bin("==", lit(kNan), lit(kNan)), false, "NaN == NaN is false") && ok;
    ok = is_bool(bin("!=", lit(kNan), lit(kNan)), true, "NaN != NaN is true") && ok;
    ok = is_bool(bin("<", lit(kNan), lit(1.0)), false, "a comparison with NaN is false") && ok;
    ok = is_bool(bin(">=", lit(kNan), lit(kNan)), false, ">= with NaN is false") && ok;
    ok = is_bool(bin("==", text("a"), text("a")), true, "string equality") && ok;
    ok = is_bool(bin("!=", text("a"), text("b")), true, "string inequality") && ok;
    ok = is_bool(bin("==", lit(true), lit(true)), true, "bool equality") && ok;
    ok =
        fails(bin("<", text("a"), text("b")), "TESSERA_IR_EVAL_TYPE", "strings do not order") && ok;
    ok =
        fails(bin("==", lit(1), text("1")), "TESSERA_IR_EVAL_TYPE", "no cross-type equality") && ok;
    return ok;
}

bool logic() {
    // `&&` and `||` short-circuit: the right side must not run, so an unknown ref is never read.
    bool ok =
        is_bool(bin("&&", lit(false), ir::make_ref("state::missing")), false, "&& short-circuits");
    ok = is_bool(bin("||", lit(true), ir::make_ref("state::missing")), true, "|| short-circuits") &&
         ok;
    ok = is_bool(bin("&&", lit(true), lit(true)), true, "true && true") && ok;
    ok = is_bool(bin("||", lit(false), lit(false)), false, "false || false") && ok;
    ok = is_bool(ir::make_unary("!", lit(false)), true, "!false") && ok;
    ok = fails(bin("&&", lit(1), lit(true)), "TESSERA_IR_EVAL_TYPE", "&& needs bools") && ok;
    ok = fails(ir::make_unary("!", lit(1)), "TESSERA_IR_EVAL_TYPE", "! needs a bool") && ok;
    ok = is_int(ir::make_conditional(lit(true), lit(1), ir::make_ref("state::missing")), 1,
                "a conditional runs only the chosen branch") &&
         ok;
    ok = fails(ir::make_conditional(lit(1), lit(1), lit(2)), "TESSERA_IR_EVAL_TYPE",
               "a conditional test must be a bool") &&
         ok;
    return ok;
}

bool functions() {
    bool ok = is_int(call("min", lit(3), lit(5)), 3, "min of ints stays int");
    ok = is_float(call("max", lit(3), lit(5.5)), 5.5, "max of mixed is a float") && ok;
    ok = is_float(call("min", lit(kNan), lit(2.0)), 2.0, "min with NaN is the other operand") && ok;
    ok = is_float(call("max", lit(1.0), lit(kNan)), 1.0, "max with NaN is the other operand") && ok;
    ok = is_float(call("max", lit(kNan), lit(kNan)), kNan, "both NaN is NaN") && ok;
    ok = is_int(call("clamp", lit(15), lit(0), lit(10)), 10, "clamp above") && ok;
    ok = is_int(call("clamp", lit(-5), lit(0), lit(10)), 0, "clamp below") && ok;
    ok = is_int(call("abs", lit(-4)), 4, "abs int") && ok;
    ok = is_int(call("abs", lit(kIntMin)), kIntMin, "abs of the minimum int stays") && ok;
    ok = is_float(call("abs", lit(-2.5)), 2.5, "abs float") && ok;
    ok = is_float(call("floor", lit(-1.5)), -2.0, "floor") && ok;
    ok = is_float(call("ceil", lit(-1.5)), -1.0, "ceil") && ok;
    ok = is_float(call("round", lit(2.5)), 3.0, "round ties away from zero (up)") && ok;
    ok = is_float(call("round", lit(-2.5)), -3.0, "round ties away from zero (down)") && ok;
    ok = is_float(call("round", lit(0.49999999999999994)), 0.0, "round just below one half") && ok;
    ok = is_int(call("toInt", lit(-2.9)), -2, "toInt truncates toward zero") && ok;
    ok = is_int(call("toInt", lit(kNan)), 0, "toInt of NaN is 0") && ok;
    ok = is_int(call("toInt", lit(1e30)), kIntMax, "toInt saturates high") && ok;
    ok = is_int(call("toInt", lit(-1e30)), kIntMin, "toInt saturates low") && ok;
    ok = is_int(call("toInt", lit(kInf)), kIntMax, "toInt of Infinity saturates") && ok;
    ok = is_float(call("toFloat", lit(7)), 7.0, "toFloat") && ok;
    ok = is_int(call("length", text("")), 0, "length of the empty string") && ok;
    // U+00E9 is two bytes, U+1EBF three, U+1F600 four: the length counts code points.
    ok = is_int(call("length", text("a\xC3\xA9\xE1\xBA\xBF\xF0\x9F\x98\x80")), 4,
                "length counts Unicode code points") &&
         ok;
    ok = fails(call("sin", lit(1.0)), "TESSERA_IR_EVAL_FUNCTION",
               "only the allowlist is callable") &&
         ok;
    ok = fails(call("min", lit(1)), "TESSERA_IR_EVAL_ARITY", "arity is checked") && ok;
    ok = fails(call("abs", text("x")), "TESSERA_IR_EVAL_TYPE", "argument types are checked") && ok;
    return ok;
}

bool number_text() {
    bool ok = expect(eval::int_text(0) == "0" && eval::int_text(-42) == "-42" &&
                         eval::int_text(kIntMin) == "-2147483648",
                     "int text is base 10");
    ok = expect(eval::float_text(1.0) == "1" && eval::float_text(0.5) == "0.5" &&
                    eval::float_text(0.1) == "0.1" && eval::float_text(100.0) == "100",
                "float text is the shortest round-trip form") &&
         ok;
    ok = expect(eval::float_text(-0.0) == "-0", "negative zero prints as -0") && ok;
    ok = expect(eval::float_text(kNan) == "NaN" && eval::float_text(kInf) == "Infinity" &&
                    eval::float_text(-kInf) == "-Infinity",
                "NaN and the infinities have fixed names") &&
         ok;
    ok = expect(eval::float_text(1e21) == "1e+21", "large values use the shortest form") && ok;

    // formatNumber: fixed, correctly rounded from the exact value, ties away from zero.
    ok = expect(eval::fixed_text(2.5, 0) == "3", "2.5 with 0 decimals rounds away") && ok;
    ok = expect(eval::fixed_text(-2.5, 0) == "-3", "-2.5 with 0 decimals rounds away") && ok;
    ok = expect(eval::fixed_text(0.5, 0) == "1", "0.5 rounds to 1") && ok;
    ok =
        expect(eval::fixed_text(0.125, 2) == "0.13", "0.125 is an exact tie and rounds away") && ok;
    // 1.005 is below the tie in binary (1.00499999999999989...), so it rounds down.
    ok = expect(eval::fixed_text(1.005, 2) == "1.00", "1.005 rounds from its exact value") && ok;
    ok = expect(eval::fixed_text(1.0, 3) == "1.000", "trailing zeros are kept") && ok;
    ok = expect(eval::fixed_text(9.999, 2) == "10.00", "a carry grows the integer part") && ok;
    ok = expect(eval::fixed_text(0.0, 2) == "0.00", "zero") && ok;
    ok = expect(eval::fixed_text(1234567.891, 1) == "1234567.9", "no thousands separator") && ok;
    ok = expect(eval::fixed_text(3.14159, 25) == eval::fixed_text(3.14159, 20),
                "decimals clamp to 20") &&
         ok;
    ok = expect(eval::fixed_text(3.14159, -3) == "3", "negative decimals clamp to 0") && ok;
    ok = expect(eval::fixed_text(kNan, 2) == "NaN" && eval::fixed_text(-kInf, 2) == "-Infinity",
                "NaN and Infinity in fixed form") &&
         ok;
    ok = expect(eval::fixed_text(-0.001, 2) == "-0.00",
                "a negative that rounds to zero keeps its sign") &&
         ok;
    ok = expect(eval::fixed_text(1e22, 0) == "10000000000000000000000", "large values are exact") &&
         ok;
    return ok;
}

bool templates_and_calls() {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part("Count: "));
    parts.push_back(ir::expr_part(ir::make_ref("state::count")));
    parts.push_back(ir::text_part(" of "));
    parts.push_back(ir::expr_part(lit(2.5)));
    const ir::ExprPtr tmpl = ir::make_template(std::move(parts));
    eval::Scope scope{{"state::count", eval::Value::of_int(3)}};
    eval::Evaluator ev;
    const auto value = run(tmpl, ev, scope);
    bool ok = expect(value && value->as_string() && *value->as_string() == "Count: 3 of 2.5",
                     "a template substitutes numbers");
    ok = is_text(call("formatNumber", lit(3.14159), lit(2)), "3.14", "formatNumber") && ok;
    ok = is_text(call("formatNumber", lit(7), lit(1)), "7.0", "formatNumber of an int") && ok;

    std::vector<ir::TemplatePart> bad;
    bad.push_back(ir::expr_part(lit(true)));
    ok = fails(ir::make_template(std::move(bad)), "TESSERA_IR_EVAL_TYPE",
               "a bool has no text form") &&
         ok;
    return ok;
}

bool scope_and_limits() {
    eval::Evaluator ev;
    const auto missing = run(ir::make_ref("state::nope"), ev);
    bool ok = expect(!missing && missing.error().code == "TESSERA_IR_EVAL_UNKNOWN_REF",
                     "an unknown ref is an error");
    ok = fails(lit(std::int64_t{1} << 40), "TESSERA_IR_EVAL_LITERAL_RANGE",
               "an int literal beyond 32 bits is rejected") &&
         ok;

    // A hostile, very deep expression must be an error, not a stack overflow.
    ir::ExprPtr deep = lit(1);
    for (int i = 0; i < 1000; ++i) {
        deep = ir::make_unary("-", std::move(deep));
    }
    ok = fails(deep, "TESSERA_IR_EVAL_DEPTH", "depth is bounded") && ok;

    eval::Evaluator lists;
    std::vector<ir::ExprPtr> items;
    items.push_back(lit(1));
    items.push_back(lit(2));
    const auto list = run(ir::make_array(std::move(items)), lists);
    ok = expect(list && list->as_list() && list->as_list()->size() == 2, "array literal") && ok;
    ok = is_int(call("length", ir::make_array([] {
                         std::vector<ir::ExprPtr> v;
                         v.push_back(lit(1));
                         v.push_back(lit(2));
                         v.push_back(lit(3));
                         return v;
                     }())),
                3, "length of a list") &&
         ok;
    return ok;
}

bool modulo_diagnostic() {
    const ir::ExprPtr first = bin("%", lit(5), lit(0));
    const ir::ExprPtr second = bin("%", lit(6), lit(0));
    eval::Evaluator ev;
    for (int i = 0; i < 3; ++i) {
        (void)run(first, ev);
    }
    bool ok = expect(ev.diagnostics().size() == 1 &&
                         ev.diagnostics()[0].code == "TESSERA_IR_INT_MODULO_ZERO",
                     "the diagnostic is reported once per expression node");
    (void)run(second, ev);
    ok = expect(ev.diagnostics().size() == 2, "another node reports again") && ok;
    eval::Evaluator fresh;
    (void)run(first, fresh);
    ok = expect(fresh.diagnostics().size() == 1, "a new session reports again") && ok;
    (void)run(bin("%", lit(5), lit(2)), fresh);
    ok = expect(fresh.diagnostics().size() == 1, "a non-zero divisor reports nothing") && ok;
    return ok;
}

bool actions() {
    eval::Scope scope{{"state::count", eval::Value::of_int(0)},
                      {"state::ratio", eval::Value::of_float(0.0)},
                      {"state::label", eval::Value::of_string("")},
                      {"anim::opacity", eval::Value::of_float(1.0)},
                      {"step", eval::Value::of_int(2)}};
    std::vector<ir::Action> list;
    list.push_back({ir::ActionKind::Set, "state::count",
                    bin("+", ir::make_ref("state::count"), ir::make_ref("step")), true});
    // A later action reads the earlier one's result, as the generated handler does.
    list.push_back({ir::ActionKind::Set, "state::ratio",
                    bin("/", ir::make_ref("state::count"), lit(4)), true});
    list.push_back({ir::ActionKind::Set, "state::label",
                    [] {
                        std::vector<ir::TemplatePart> parts;
                        parts.push_back(ir::text_part("n="));
                        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
                        return ir::make_template(std::move(parts));
                    }(),
                    true});
    list.push_back({ir::ActionKind::AnimSet, "anim::opacity", lit(0.5), false});
    eval::Evaluator ev;
    bool ok = expect(ev.run(list, scope).has_value(), "the action list runs");
    ok = expect(*scope["state::count"].as_int() == 2 && *scope["state::ratio"].as_float() == 0.5 &&
                    *scope["state::label"].as_string() == "n=2" &&
                    *scope["anim::opacity"].as_float() == 0.5,
                "actions apply in order and see each other") &&
         ok;

    // An int widens into a float state, as C++ converts it; other type changes are errors.
    std::vector<ir::Action> widen;
    widen.push_back({ir::ActionKind::Set, "state::ratio", lit(3), true});
    ok = expect(ev.run(widen, scope).has_value() && *scope["state::ratio"].as_float() == 3.0,
                "an int widens into a float state") &&
         ok;
    std::vector<ir::Action> wrong_type;
    wrong_type.push_back({ir::ActionKind::Set, "state::count", text("x"), true});
    const auto type_error = ev.run(wrong_type, scope);
    ok = expect(!type_error && type_error.error().code == "TESSERA_IR_EVAL_TYPE" &&
                    *scope["state::count"].as_int() == 2,
                "assigning another type is an error and leaves the state alone") &&
         ok;
    std::vector<ir::Action> unknown;
    unknown.push_back({ir::ActionKind::Set, "state::nope", lit(1), true});
    ok = expect(!ev.run(unknown, scope) &&
                    ev.run(unknown, scope).error().code == "TESSERA_IR_EVAL_UNKNOWN_REF",
                "an undeclared target is an error") &&
         ok;
    std::vector<ir::Action> bad_target;
    bad_target.push_back({ir::ActionKind::Set, "anim::opacity", lit(1.0), true});
    ok = expect(!ev.run(bad_target, scope) &&
                    ev.run(bad_target, scope).error().code == "TESSERA_IR_EVAL_ACTION_TARGET",
                "a set may only write state") &&
         ok;

    // identical(): NaN matches NaN, zero signs differ, types differ.
    ok = expect(eval::identical(eval::Value::of_float(kNan), eval::Value::of_float(kNan)),
                "two NaNs are identical") &&
         ok;
    ok = expect(!eval::identical(eval::Value::of_float(0.0), eval::Value::of_float(-0.0)),
                "0.0 and -0.0 are not identical") &&
         ok;
    ok = expect(!eval::identical(eval::Value::of_int(1), eval::Value::of_float(1.0)),
                "an int is not identical to a float") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = integers();
    ok = floats() && ok;
    ok = comparisons() && ok;
    ok = logic() && ok;
    ok = functions() && ok;
    ok = number_text() && ok;
    ok = templates_and_calls() && ok;
    ok = scope_and_limits() && ok;
    ok = modulo_diagnostic() && ok;
    ok = actions() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "ir_eval: ok\n";
    return 0;
}
