// SPDX-License-Identifier: MIT

#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <utility>

namespace tessera::compiler::ir {

std::string_view node_kind_name(NodeKind kind) {
    switch (kind) {
    case NodeKind::View:
        return "View";
    case NodeKind::Text:
        return "Text";
    case NodeKind::Button:
        return "Button";
    case NodeKind::TextInput:
        return "TextInput";
    case NodeKind::TextArea:
        return "TextArea";
    case NodeKind::Image:
        return "Image";
    case NodeKind::Path:
        return "Path";
    case NodeKind::Scroll:
        return "Scroll";
    case NodeKind::Spacer:
        return "Spacer";
    case NodeKind::VirtualList:
        return "VirtualList";
    case NodeKind::VirtualGrid:
        return "VirtualGrid";
    case NodeKind::ErrorBoundary:
        return "ErrorBoundary";
    case NodeKind::Suspense:
        return "Suspense";
    case NodeKind::CustomElement:
        return "CustomElement";
    case NodeKind::Component:
        return "Component";
    case NodeKind::Slot:
        return "Slot";
    }
    return "";
}

std::string_view binding_kind_name(BindingKind kind) {
    switch (kind) {
    case BindingKind::State:
        return "state";
    case BindingKind::Effect:
        return "effect";
    case BindingKind::Ref:
        return "ref";
    case BindingKind::Animated:
        return "animated";
    case BindingKind::Event:
        return "event";
    case BindingKind::Logic:
        return "logic";
    }
    return "";
}

namespace {

ExprPtr make_expr(ExprKind kind) {
    auto expr = std::make_unique<Expr>();
    expr->kind = kind;
    return expr;
}

} // namespace

ExprPtr make_literal(LiteralValue value) {
    auto expr = make_expr(ExprKind::Literal);
    expr->literal = std::move(value);
    return expr;
}

ExprPtr make_ref(std::string id) {
    auto expr = make_expr(ExprKind::Ref);
    expr->id = std::move(id);
    return expr;
}

ExprPtr make_binary(std::string op, ExprPtr left, ExprPtr right) {
    auto expr = make_expr(ExprKind::Binary);
    expr->op = std::move(op);
    expr->operands.push_back(std::move(left));
    expr->operands.push_back(std::move(right));
    return expr;
}

ExprPtr make_unary(std::string op, ExprPtr value) {
    auto expr = make_expr(ExprKind::Unary);
    expr->op = std::move(op);
    expr->operands.push_back(std::move(value));
    return expr;
}

ExprPtr make_conditional(ExprPtr test, ExprPtr then_value, ExprPtr else_value) {
    auto expr = make_expr(ExprKind::Conditional);
    expr->operands.push_back(std::move(test));
    expr->operands.push_back(std::move(then_value));
    expr->operands.push_back(std::move(else_value));
    return expr;
}

ExprPtr make_call(std::string callee, std::vector<ExprPtr> args) {
    auto expr = make_expr(ExprKind::Call);
    expr->id = std::move(callee);
    expr->operands = std::move(args);
    return expr;
}

ExprPtr make_template(std::vector<TemplatePart> parts) {
    auto expr = make_expr(ExprKind::Template);
    expr->parts = std::move(parts);
    return expr;
}

ExprPtr make_array(std::vector<ExprPtr> elements) {
    auto expr = make_expr(ExprKind::Array);
    expr->operands = std::move(elements);
    return expr;
}

ExprPtr make_record(std::string type, std::vector<RecordField> fields) {
    auto expr = make_expr(ExprKind::Record);
    expr->type = std::move(type);
    expr->fields = std::move(fields);
    return expr;
}

ExprPtr make_optional_field(ExprPtr base, std::string field) {
    auto expr = make_expr(ExprKind::OptionalField);
    expr->id = std::move(field);
    expr->operands.push_back(std::move(base));
    return expr;
}

ExprPtr make_coalesce(ExprPtr value, ExprPtr fallback) {
    auto expr = make_expr(ExprKind::Coalesce);
    expr->operands.push_back(std::move(value));
    expr->operands.push_back(std::move(fallback));
    return expr;
}

ExprPtr make_list_filter(ExprPtr list, std::string item, ExprPtr predicate) {
    auto expr = make_expr(ExprKind::ListFilter);
    expr->id = std::move(item);
    expr->operands.push_back(std::move(list));
    expr->operands.push_back(std::move(predicate));
    return expr;
}

ExprPtr make_list_slice(ExprPtr list, std::vector<ExprPtr> bounds) {
    auto expr = make_expr(ExprKind::ListSlice);
    expr->operands.push_back(std::move(list));
    for (ExprPtr& bound : bounds) {
        expr->operands.push_back(std::move(bound));
    }
    return expr;
}

ExprPtr make_record_update(std::string type, std::string base, std::vector<RecordField> fields) {
    auto expr = make_expr(ExprKind::RecordUpdate);
    expr->type = std::move(type);
    expr->id = std::move(base);
    expr->fields = std::move(fields);
    return expr;
}

bool is_list_source(const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Ref:
        return true;
    case ExprKind::ListFilter:
        // `list.filter(item => <bool>)`: operand 0 is the list; a malformed operand count is the
        // validator's error, not a different source rule.
        return expr.operands.size() == 2 && expr.operands[0] != nullptr &&
               is_list_source(*expr.operands[0]);
    case ExprKind::ListSlice:
        return !expr.operands.empty() && expr.operands[0] != nullptr &&
               is_list_source(*expr.operands[0]);
    default:
        return false;
    }
}

const Expr* list_source_ref(const Expr& expr) {
    const Expr* current = &expr;
    while (current != nullptr && current->kind != ExprKind::Ref) {
        if (!is_list_source(*current) || current->operands.empty()) {
            return nullptr;
        }
        current = current->operands[0].get();
    }
    return current;
}

TemplatePart text_part(std::string text) {
    return TemplatePart{std::move(text), nullptr};
}

TemplatePart expr_part(ExprPtr expr) {
    return TemplatePart{{}, std::move(expr)};
}

} // namespace tessera::compiler::ir
