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
    case NodeKind::CustomElement:
        return "CustomElement";
    case NodeKind::Component:
        return "Component";
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

TemplatePart text_part(std::string text) {
    return TemplatePart{std::move(text), nullptr};
}

TemplatePart expr_part(ExprPtr expr) {
    return TemplatePart{{}, std::move(expr)};
}

} // namespace tessera::compiler::ir
