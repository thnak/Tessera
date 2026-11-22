// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace tessera::compiler::ir {

// docs/specs/ir-v1.md section 4.
//
// `OptionalField` and `Coalesce` are the two additive kinds of ADR-066 item 1 (`a?.b` and `a ?? b`
// over an `optional<T>`). Both are optional in the format: a module that writes neither is
// byte-identical to the IR before them, so `irVersion` stays 1 (ir-v1-expr.md section 12).
enum class ExprKind : std::uint8_t {
    Literal,
    Ref,
    Binary,
    Unary,
    Conditional,
    Template,
    Call,
    Array,
    Record,
    // `a?.b`: `operands[0]` is the optional base, `id` the field name. The result is `optional<T>`
    // of the field's type: absent when the base is absent, otherwise the field.
    OptionalField,
    // `a ?? b`: `operands[0]` is the optional value, `operands[1]` the fallback. The result is the
    // inner type of the optional.
    Coalesce,
};

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

// One field of a `record` expression (section 4.3): the member name and its value. Fields keep
// member order; a value position literal may be written bare only in a JSON value position, so a
// field value is always an expression object in the writer (like any other operand).
struct RecordField {
    std::string name;
    ExprPtr value;
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
    std::string type;                // Record: the declared record name
    std::vector<RecordField> fields; // Record: one entry per member value, in member order
};

ExprPtr make_literal(LiteralValue value);
ExprPtr make_ref(std::string id);
ExprPtr make_binary(std::string op, ExprPtr left, ExprPtr right);
ExprPtr make_unary(std::string op, ExprPtr value);
ExprPtr make_conditional(ExprPtr test, ExprPtr then_value, ExprPtr else_value);
ExprPtr make_call(std::string callee, std::vector<ExprPtr> args);
ExprPtr make_template(std::vector<TemplatePart> parts);
// Array (list) literal: one operand per element (docs/specs/ir-v1.md section 4). Lowering accepts
// literal elements only, and only where the subset spec allows an array literal (a `useState`
// initial of a declared `T[]` type, a list-loop source or a top-level constant).
ExprPtr make_array(std::vector<ExprPtr> elements);
// Record value (section 4.3, ADR-051): a declared record name plus one field per written member,
// in member order. The first slice accepts literal, nested-record and array-of-literal field values
// only; the validator is what enforces that and the field set.
ExprPtr make_record(std::string type, std::vector<RecordField> fields);
// `a?.b` (ADR-066 item 1): the optional base and the field name. The result is an `optional<T>` of
// the field's type, absent when the base is absent.
ExprPtr make_optional_field(ExprPtr base, std::string field);
// `a ?? b` (ADR-066 item 1): the optional value and the fallback. The result is the optional's
// inner type.
ExprPtr make_coalesce(ExprPtr value, ExprPtr fallback);

TemplatePart text_part(std::string text);
TemplatePart expr_part(ExprPtr expr);

} // namespace tessera::compiler::ir
