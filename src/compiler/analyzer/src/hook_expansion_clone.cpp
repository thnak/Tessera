// SPDX-License-Identifier: MIT

// AST helpers of the custom-hook expansion pass (spike #659, ADR-065): deep copies, identifier
// substitution and renaming. Every helper is a pure function of its input, so the expansion is
// deterministic and the module's hook declarations are never modified.

#include "hook_expansion_internal.hpp"

#include <utility>

namespace tessera::compiler::jsx::internal {

ExprPtr clone_expr(const Expr& expr) {
    auto copy = std::make_unique<Expr>();
    copy->kind = expr.kind;
    copy->span = expr.span;
    copy->height = expr.height;
    copy->text = expr.text;
    copy->number = expr.number;
    copy->boolean = expr.boolean;
    copy->template_strings = expr.template_strings;
    copy->params = expr.params;
    copy->arrow_block_body = expr.arrow_block_body;
    copy->operands.reserve(expr.operands.size());
    for (const ExprPtr& operand : expr.operands) {
        copy->operands.push_back(operand ? clone_expr(*operand) : nullptr);
    }
    copy->properties.reserve(expr.properties.size());
    for (const ObjectProperty& property : expr.properties) {
        ObjectProperty entry;
        entry.key = property.key;
        entry.key_is_string = property.key_is_string;
        entry.key_span = property.key_span;
        entry.value = property.value ? clone_expr(*property.value) : nullptr;
        copy->properties.push_back(std::move(entry));
    }
    copy->param_types.reserve(expr.param_types.size());
    for (const TypePtr& type : expr.param_types) {
        copy->param_types.push_back(type ? clone_type(*type) : nullptr);
    }
    copy->return_type = expr.return_type ? clone_type(*expr.return_type) : nullptr;
    copy->type_args.reserve(expr.type_args.size());
    for (const TypePtr& type : expr.type_args) {
        copy->type_args.push_back(type ? clone_type(*type) : nullptr);
    }
    copy->jsx = expr.jsx ? clone_jsx(*expr.jsx) : nullptr;
    return copy;
}

JsxPtr clone_jsx(const JsxNode& node) {
    auto copy = std::make_unique<JsxNode>();
    copy->kind = node.kind;
    copy->span = node.span;
    copy->height = node.height;
    copy->tag = node.tag;
    copy->tag_span = node.tag_span;
    copy->self_closing = node.self_closing;
    copy->text = node.text;
    copy->attributes.reserve(node.attributes.size());
    for (const JsxAttribute& attribute : node.attributes) {
        JsxAttribute entry;
        entry.name = attribute.name;
        entry.name_span = attribute.name_span;
        entry.kind = attribute.kind;
        entry.string_value = attribute.string_value;
        entry.expr = attribute.expr ? clone_expr(*attribute.expr) : nullptr;
        entry.span = attribute.span;
        copy->attributes.push_back(std::move(entry));
    }
    copy->children.reserve(node.children.size());
    for (const JsxPtr& child : node.children) {
        copy->children.push_back(child ? clone_jsx(*child) : nullptr);
    }
    copy->expr = node.expr ? clone_expr(*node.expr) : nullptr;
    return copy;
}

TypePtr clone_type(const TypeNode& type) {
    auto copy = std::make_unique<TypeNode>();
    copy->kind = type.kind;
    copy->span = type.span;
    copy->height = type.height;
    copy->text = type.text;
    copy->param_names = type.param_names;
    copy->children.reserve(type.children.size());
    for (const TypePtr& child : type.children) {
        copy->children.push_back(child ? clone_type(*child) : nullptr);
    }
    copy->members.reserve(type.members.size());
    for (const TypeMember& member : type.members) {
        TypeMember entry;
        entry.name = member.name;
        entry.optional = member.optional;
        entry.span = member.span;
        entry.type = member.type ? clone_type(*member.type) : nullptr;
        copy->members.push_back(std::move(entry));
    }
    return copy;
}

Statement clone_statement(const Statement& statement) {
    Statement copy;
    copy.kind = statement.kind;
    copy.span = statement.span;
    copy.names = statement.names;
    copy.array_pattern = statement.array_pattern;
    copy.object_pattern = statement.object_pattern;
    copy.type = statement.type ? clone_type(*statement.type) : nullptr;
    copy.init = statement.init ? clone_expr(*statement.init) : nullptr;
    copy.bind_id = statement.bind_id;
    copy.bind_span = statement.bind_span;
    // Issue #829: a memo/callback inside a hook body keeps its recorded dependency list, so the
    // rules pass checks the list at the expanded position. Only plain names are stored, so nothing
    // here needs a rename or a substitution.
    copy.memo_hook = statement.memo_hook;
    copy.memo_deps = statement.memo_deps;
    copy.memo_deps_span = statement.memo_deps_span;
    copy.memo_deps_error_span = statement.memo_deps_error_span;
    return copy;
}

} // namespace tessera::compiler::jsx::internal
