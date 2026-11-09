// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/source_map.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Parser output (docs/specs/jsx-subset-v1.md section 16.1). The AST is internal to the front end:
// the IR and code generation never include this header (AGENTS.md non-negotiable 4).
namespace tessera::compiler::jsx {

struct Expr;
struct JsxNode;
using ExprPtr = std::unique_ptr<Expr>;
using JsxPtr = std::unique_ptr<JsxNode>;

enum class ExprKind {
    Number,      // text = source spelling, number = value
    String,      // text = decoded contents
    Template,    // template_strings (cooked) interleaved with operands (substitutions)
    Boolean,     // boolean
    Null,        //
    Identifier,  // text = name
    Unary,       // text = operator, operands = [operand]
    Binary,      // text = operator, operands = [lhs, rhs]; also assignments the subset rejects
    Conditional, // operands = [test, then, else]
    Member,      // text = property name, operands = [object]; `a[i]` has text "[]" and [a, i]
    Call,        // operands = [callee, arguments...]
    Array,       // operands = elements
    Object,      // properties
    Jsx,         // jsx
    Arrow,       // params; operands = [body] or the statements of a block body
};

struct ObjectProperty {
    std::string key; // decoded
    bool key_is_string = false;
    Span key_span;
    ExprPtr value;
};

// One node type for every expression so no RTTI or visitor machinery is needed. The comment on
// each ExprKind says which fields it uses; the others stay empty.
struct Expr {
    ExprKind kind = ExprKind::Null;
    Span span;
    // 1 + the largest height among children. The parser rejects heights above
    // Limits::max_nesting_depth, which bounds recursion in every later pass and in destruction.
    std::uint32_t height = 1;
    std::string text;
    double number = 0.0;
    bool boolean = false;
    std::vector<ExprPtr> operands;
    std::vector<std::string> template_strings;
    std::vector<ObjectProperty> properties;
    std::vector<std::string> params;
    bool arrow_block_body = false;
    JsxPtr jsx;
};

enum class JsxKind {
    Element,       // tag, attributes, children
    Fragment,      // children
    Text,          // text (already whitespace-collapsed and entity-decoded)
    ExprContainer, // expr; null for `{}` and `{/* comment */}`
};

enum class JsxAttrKind {
    Flag,       // `disabled`
    String,     // `a="x"`, string_value decoded
    Expression, // `a={expr}`
    Spread,     // `{...expr}`; name is empty
};

struct JsxAttribute {
    std::string name;
    Span name_span;
    JsxAttrKind kind = JsxAttrKind::Flag;
    std::string string_value;
    ExprPtr expr;
    Span span;
};

struct JsxNode {
    JsxKind kind = JsxKind::Element;
    Span span;
    std::uint32_t height = 1;
    std::string tag; // Element: as written, including `.`/`:` of tags the subset rejects
    Span tag_span;
    bool self_closing = false;
    std::vector<JsxAttribute> attributes;
    std::vector<JsxPtr> children;
    std::string text;
    ExprPtr expr;
};

} // namespace tessera::compiler::jsx
