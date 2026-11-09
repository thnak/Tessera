// SPDX-License-Identifier: MIT

// IR validation of expressions and types.

#include "ir_validate_util.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ir_test;

void test_expressions() {
    const auto set_text = [](ir::IrModule& m, ir::ExprPtr text) {
        label_node(m).text = std::move(text);
    };
    const auto ref = [] { return ir::make_ref("state::n"); };
    expect(
        "every binary operator is accepted",
        [&](auto& m) {
            for (const char* op :
                 {"+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||"}) {
                list(m).root.props.emplace(std::string("p") + op,
                                           ir::make_binary(op, ref(), ref()));
            }
        },
        "");
    expect(
        "unknown binary operator",
        [&](auto& m) { set_text(m, ir::make_binary("**", ref(), ref())); }, kUnsupported);
    expect(
        "loose equality is not an IR operator",
        [&](auto& m) { set_text(m, ir::make_binary("===", ref(), ref())); }, kUnsupported);
    expect(
        "every unary operator is accepted",
        [&](auto& m) {
            for (const char* op : {"!", "-", "+"}) {
                list(m).root.props.emplace(std::string("u") + op, ir::make_unary(op, ref()));
            }
        },
        "");
    expect(
        "unknown unary operator", [&](auto& m) { set_text(m, ir::make_unary("~", ref())); },
        kUnsupported);
    expect(
        "every allowlisted function is accepted",
        [&](auto& m) {
            for (const char* fn : {"min", "max", "clamp", "abs", "floor", "ceil", "round", "toInt",
                                   "toFloat", "length", "formatNumber"}) {
                std::vector<ir::ExprPtr> args;
                args.push_back(ref());
                list(m).root.props.emplace(std::string("f") + fn,
                                           ir::make_call(fn, std::move(args)));
            }
        },
        "");
    expect(
        "function off the allowlist", [&](auto& m) { set_text(m, ir::make_call("Math.sin", {})); },
        kUnsupported);
    expect(
        "binary with one operand",
        [&](auto& m) {
            auto bad = ir::make_binary("+", ref(), ref());
            bad->operands.pop_back();
            set_text(m, std::move(bad));
        },
        kMalformed);
    expect(
        "binary with a missing operand",
        [&](auto& m) { set_text(m, ir::make_binary("+", ref(), nullptr)); }, kMalformed);
    expect(
        "conditional with two operands",
        [&](auto& m) {
            auto bad = ir::make_conditional(ref(), ref(), ref());
            bad->operands.pop_back();
            set_text(m, std::move(bad));
        },
        kMalformed);
    expect(
        "unary with two operands",
        [&](auto& m) {
            auto bad = ir::make_unary("!", ref());
            bad->operands.push_back(ref());
            set_text(m, std::move(bad));
        },
        kMalformed);
    expect(
        "template parts are checked",
        [&](auto& m) {
            std::vector<ir::TemplatePart> parts;
            parts.push_back(ir::text_part("a"));
            parts.push_back(ir::expr_part(ir::make_ref("state::gone")));
            set_text(m, ir::make_template(std::move(parts)));
        },
        kUnresolved);
    expect(
        "call arguments are checked",
        [&](auto& m) {
            std::vector<ir::ExprPtr> args;
            args.push_back(ir::make_ref("state::gone"));
            set_text(m, ir::make_call("abs", std::move(args)));
        },
        kUnresolved);
    expect(
        "prop expressions are checked",
        [&](auto& m) { row_node(m).props["label"] = ir::make_ref("zzz"); }, kUnresolved);
    expect(
        "style expressions are checked",
        [&](auto& m) { label_node(m).style["width"] = ir::make_ref("zzz"); }, kUnresolved);
    expect(
        "conditions are checked", [&](auto& m) { label_node(m).condition = ir::make_ref("zzz"); },
        kUnresolved);
    expect("a null prop value", [&](auto& m) { label_node(m).props["x"] = nullptr; }, kMalformed);
    expect(
        "non-finite float literal",
        [&](auto& m) { set_text(m, ir::make_literal(std::numeric_limits<double>::infinity())); },
        "TESSERA_IR_LITERAL;");
}

void test_types() {
    for (const char* type :
         {"bool", "int", "float", "string", "color", "length", "node_ref", "list<int>",
          "optional<string>", "list<optional<int>>", "Row", "list<Row>"}) {
        expect(
            std::string("accepted type ") + type, [&](auto& m) { list(m).state[0].type = type; },
            "");
    }
    for (const char* type :
         {"", "list<", "list<>", "list<int", "optional<>>", "1x", "a b", "int[]", "list<a-b>"}) {
        expect(
            std::string("rejected type `") + type + "`",
            [&](auto& m) { list(m).state[0].type = type; }, kUnsupported);
    }
    expect("prop type", [](auto& m) { list(m).props[0].type = "x y"; }, kUnsupported);
    expect("environment type", [](auto& m) { list(m).environment[0].type = ""; }, kUnsupported);
    expect(
        "handler parameter type", [](auto& m) { list(m).handlers[0].params[0].type = "x y"; },
        kUnsupported);
}

} // namespace

int main() {
    test_expressions();
    test_types();
    return finish("ir validate expression tests");
}
