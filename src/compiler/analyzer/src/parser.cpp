// SPDX-License-Identifier: MIT

#include "parser_internal.hpp"

#include <algorithm>

namespace tessera::compiler::jsx {

Parser::Parser(std::string_view source, const Limits& limits)
    : limits_(limits),
      map_(source),
      sink_(map_, limits_),
      lexer_(source, sink_, limits_) {}

void Parser::advance(LexMode mode) {
    prev_end_ = cur_.span.end();
    cur_ = lexer_.next(mode);
}

void Parser::advance_template() {
    prev_end_ = cur_.span.end();
    cur_ = lexer_.continue_template(cur_);
}

Parser::Checkpoint Parser::checkpoint() const {
    return Checkpoint{lexer_.mark(), sink_.mark(), cur_, prev_end_};
}

void Parser::restore(Checkpoint saved) {
    lexer_.rewind(saved.lexer);
    sink_.rewind(saved.sink);
    cur_ = std::move(saved.cur);
    prev_end_ = saved.prev_end;
}

void Parser::report(std::string_view code, std::string message, Span span, std::string suggestion) {
    sink_.error(code, std::move(message), span, std::move(suggestion));
}

void Parser::fail(std::string_view code, std::string message, Span span, std::string suggestion) {
    // After the lexer hit a limit it reported it; the parse errors that follow from the missing
    // tokens would only be noise.
    if (!fatal_ && !lexer_.stopped()) {
        report(code, std::move(message), span, std::move(suggestion));
    }
    fatal_ = true;
}

std::string Parser::describe_current() const {
    if (cur_.kind == TokenKind::Eof) {
        return "end of input";
    }
    constexpr std::size_t kMaxShown = 24;
    std::string shown(cur_.text.substr(0, kMaxShown));
    if (cur_.text.size() > kMaxShown) {
        shown += "...";
    }
    return "`" + shown + "`";
}

void Parser::fail_unexpected(std::string_view expected) {
    fail("TESSERA_JSX_PARSE_UNEXPECTED_TOKEN",
         "unexpected " + describe_current() + ", expected " + std::string(expected), cur_.span);
}

bool Parser::expect_punct(std::string_view punctuator, LexMode after) {
    if (fatal_) {
        return false;
    }
    if (!at_punct(punctuator)) {
        fail_unexpected("`" + std::string(punctuator) + "`");
        return false;
    }
    advance(after);
    return true;
}

bool Parser::enter() {
    if (depth_ >= limits_.max_nesting_depth) {
        fail("TESSERA_JSX_LIMIT_DEPTH",
             "nesting is deeper than " + std::to_string(limits_.max_nesting_depth) + " levels",
             cur_.span, "split the expression or the element tree");
        return false;
    }
    ++depth_;
    return true;
}

bool Parser::finish_node(Expr& expr) {
    std::uint32_t tallest = 0;
    for (const auto& operand : expr.operands) {
        tallest = std::max(tallest, operand->height);
    }
    for (const auto& property : expr.properties) {
        tallest = std::max(tallest, property.value->height);
    }
    if (expr.jsx) {
        tallest = std::max(tallest, expr.jsx->height);
    }
    expr.height = tallest + 1;
    if (expr.height > limits_.max_nesting_depth) {
        fail("TESSERA_JSX_LIMIT_DEPTH",
             "expression is deeper than " + std::to_string(limits_.max_nesting_depth) + " levels",
             expr.span, "split the expression");
        return false;
    }
    return true;
}

bool Parser::finish_node(JsxNode& node) {
    std::uint32_t tallest = 0;
    for (const auto& child : node.children) {
        tallest = std::max(tallest, child->height);
    }
    for (const auto& attribute : node.attributes) {
        if (attribute.expr) {
            tallest = std::max(tallest, attribute.expr->height);
        }
    }
    if (node.expr) {
        tallest = std::max(tallest, node.expr->height);
    }
    node.height = tallest + 1;
    if (node.height > limits_.max_nesting_depth) {
        fail("TESSERA_JSX_LIMIT_DEPTH",
             "element tree is deeper than " + std::to_string(limits_.max_nesting_depth) + " levels",
             node.span, "split the element tree");
        return false;
    }
    return true;
}

JsxPtr Parser::parse_jsx_root() {
    advance(LexMode::Code);
    if (!at_punct("<")) {
        fail_unexpected("a JSX element");
        return nullptr;
    }
    JsxPtr root = parse_jsx_element(LexMode::Code);
    if (!fatal_ && cur_.kind != TokenKind::Eof) {
        fail_unexpected("end of input after the element");
    }
    return fatal_ ? nullptr : std::move(root);
}

ExprPtr Parser::parse_expression_root() {
    advance(LexMode::Code);
    ExprPtr root = parse_expr();
    if (!fatal_ && cur_.kind != TokenKind::Eof) {
        fail_unexpected("end of input after the expression");
    }
    return fatal_ ? nullptr : std::move(root);
}

JsxParseResult parse_jsx_element(std::string_view source, const Limits& limits) {
    Parser parser(source, limits);
    JsxPtr root = parser.parse_jsx_root();
    return parser.finish(std::move(root));
}

ExpressionParseResult parse_expression(std::string_view source, const Limits& limits) {
    Parser parser(source, limits);
    ExprPtr root = parser.parse_expression_root();
    return parser.finish(std::move(root));
}

} // namespace tessera::compiler::jsx
