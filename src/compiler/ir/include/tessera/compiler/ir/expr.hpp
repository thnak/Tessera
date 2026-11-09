// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace tessera::compiler::ir {

// docs/specs/ir-v1.md section 4.
enum class ExprKind { Literal, Ref, Binary, Unary, Conditional, Template, Call };

struct Null {};
// A literal is null, bool, int, float or string. Int and float stay distinct so the writer can
// print `1` and `1.0` differently.
using LiteralValue = std::variant<Null, bool, std::int64_t, double, std::string>;

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// One piece of a template: literal text when `expr` is null, otherwise an expression.
struct TemplatePart {
    std::string text;
    ExprPtr expr;
};

// Operand order: binary {left, right}; unary {value}; conditional {test, then, else};
// call {args...}. A malformed operand count is reported by the validator, not by the type.
struct Expr {
    ExprKind kind = ExprKind::Literal;
    LiteralValue literal;            // Literal
    std::string id;                  // Ref: reference id; Call: callee
    std::string op;                  // Binary, Unary
    std::vector<ExprPtr> operands;   // Binary, Unary, Conditional, Call
    std::vector<TemplatePart> parts; // Template
};

ExprPtr make_literal(LiteralValue value);
ExprPtr make_ref(std::string id);
ExprPtr make_binary(std::string op, ExprPtr left, ExprPtr right);
ExprPtr make_unary(std::string op, ExprPtr value);
ExprPtr make_conditional(ExprPtr test, ExprPtr then_value, ExprPtr else_value);
ExprPtr make_call(std::string callee, std::vector<ExprPtr> args);
ExprPtr make_template(std::vector<TemplatePart> parts);

TemplatePart text_part(std::string text);
TemplatePart expr_part(ExprPtr expr);

} // namespace tessera::compiler::ir
