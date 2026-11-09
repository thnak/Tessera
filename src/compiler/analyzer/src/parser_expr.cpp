// SPDX-License-Identifier: MIT

// Operators and postfix forms of docs/specs/jsx-subset-v1.md section 8.1. Operators the subset
// rejects are still parsed (maximal munch in the lexer, a precedence slot here) so the parser can
// report the specific section 12 code and keep finding independent errors.

#include "parser_internal.hpp"

#include <array>

namespace tessera::compiler::jsx {
namespace {

struct BinaryOperator {
    int precedence = 0; // 0: not a binary operator
    std::string_view code;
    std::string_view message;
    std::string_view suggestion;
};

constexpr std::string_view kOperatorCode = "TESSERA_JSX_SUBSET_OPERATOR";
constexpr std::string_view kNotAvailable = "not available in V1";

BinaryOperator binary_operator(const Token& token) {
    if (token.kind == TokenKind::Keyword) {
        if (token.text == "in" || token.text == "instanceof") {
            return {5, kOperatorCode, "`in` and `instanceof` are not supported", kNotAvailable};
        }
        return {};
    }
    if (token.kind != TokenKind::Punct) {
        return {};
    }
    const std::string_view op = token.text;
    if (op == "||") {
        return {1, {}, {}, {}};
    }
    if (op == "&&") {
        return {2, {}, {}, {}};
    }
    if (op == "===" || op == "!==") {
        return {4, {}, {}, {}};
    }
    if (op == "==" || op == "!=") {
        return {4, "TESSERA_JSX_SUBSET_LOOSE_EQUALITY", "loose equality is not supported",
                op == "==" ? "use `===`" : "use `!==`"};
    }
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        return {5, {}, {}, {}};
    }
    if (op == "+" || op == "-") {
        return {7, {}, {}, {}};
    }
    if (op == "*" || op == "/" || op == "%") {
        return {8, {}, {}, {}};
    }
    if (op == "??") {
        return {1, kOperatorCode, "`??` is not supported", kNotAvailable};
    }
    if (op == "|" || op == "^" || op == "&") {
        return {3, kOperatorCode, "bitwise operators are not supported", kNotAvailable};
    }
    if (op == "<<" || op == ">>" || op == ">>>") {
        return {6, kOperatorCode, "bitwise operators are not supported", kNotAvailable};
    }
    if (op == "**") {
        return {9, kOperatorCode, "`**` is not supported", kNotAvailable};
    }
    return {};
}

bool is_assignment(const Token& token) {
    static constexpr std::array<std::string_view, 16> kAssignments = {
        "=",   "+=",   "-=", "*=", "/=", "%=",  "**=", "<<=",
        ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "?\?="};
    if (token.kind != TokenKind::Punct) {
        return false;
    }
    for (const auto assignment : kAssignments) {
        if (token.text == assignment) {
            return true;
        }
    }
    return false;
}

} // namespace

ExprPtr Parser::make_expr(ExprKind kind, std::uint32_t begin) {
    auto expr = std::make_unique<Expr>();
    expr->kind = kind;
    expr->span = span_from(begin);
    return expr;
}

ExprPtr Parser::make_operator(ExprKind kind, std::uint32_t begin, std::string op,
                              std::vector<ExprPtr> operands) {
    auto expr = make_expr(kind, begin);
    expr->text = std::move(op);
    expr->operands = std::move(operands);
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

ExprPtr Parser::parse_expr() {
    const DepthScope scope(*this);
    if (!scope.entered()) {
        return nullptr;
    }
    ExprPtr expr = parse_conditional();
    if (!expr || fatal_ || !is_assignment(cur_)) {
        return fatal_ ? nullptr : std::move(expr);
    }
    const std::uint32_t begin = expr->span.offset;
    std::string op(cur_.text);
    report("TESSERA_JSX_SUBSET_MUTATION", "assignment `" + op + "` is not supported", cur_.span,
           "use `const` and state setters");
    advance(LexMode::Code);
    ExprPtr value = parse_expr();
    if (!value) {
        return nullptr;
    }
    std::vector<ExprPtr> operands;
    operands.push_back(std::move(expr));
    operands.push_back(std::move(value));
    return make_operator(ExprKind::Binary, begin, std::move(op), std::move(operands));
}

ExprPtr Parser::parse_conditional() {
    ExprPtr test = parse_binary(1);
    if (!test || fatal_ || !at_punct("?")) {
        return fatal_ ? nullptr : std::move(test);
    }
    const std::uint32_t begin = test->span.offset;
    advance(LexMode::Code);
    ExprPtr when_true = parse_expr();
    if (!when_true || !expect_punct(":", LexMode::Code)) {
        return nullptr;
    }
    ExprPtr when_false = parse_expr();
    if (!when_false) {
        return nullptr;
    }
    std::vector<ExprPtr> operands;
    operands.push_back(std::move(test));
    operands.push_back(std::move(when_true));
    operands.push_back(std::move(when_false));
    return make_operator(ExprKind::Conditional, begin, "?:", std::move(operands));
}

ExprPtr Parser::parse_binary(int min_precedence) {
    ExprPtr lhs = parse_unary();
    while (lhs && !fatal_) {
        const BinaryOperator info = binary_operator(cur_);
        if (info.precedence == 0 || info.precedence < min_precedence) {
            break;
        }
        if (!info.code.empty()) {
            report(info.code, std::string(info.message), cur_.span, std::string(info.suggestion));
        }
        const std::uint32_t begin = lhs->span.offset;
        std::string op(cur_.text);
        advance(LexMode::Code);
        ExprPtr rhs = parse_binary(info.precedence + 1);
        if (!rhs) {
            return nullptr;
        }
        std::vector<ExprPtr> operands;
        operands.push_back(std::move(lhs));
        operands.push_back(std::move(rhs));
        lhs = make_operator(ExprKind::Binary, begin, std::move(op), std::move(operands));
    }
    return fatal_ ? nullptr : std::move(lhs);
}

ExprPtr Parser::parse_unary() {
    const DepthScope scope(*this);
    if (!scope.entered()) {
        return nullptr;
    }
    const std::uint32_t begin = cur_.span.offset;
    const std::string_view text = cur_.text;
    bool is_unary = false;
    if (cur_.kind == TokenKind::Punct) {
        if (text == "!" || text == "-" || text == "+") {
            is_unary = true;
        } else if (text == "++" || text == "--") {
            report("TESSERA_JSX_SUBSET_MUTATION", "`" + std::string(text) + "` is not supported",
                   cur_.span, "use `const` and state setters");
            is_unary = true;
        } else if (text == "~") {
            report(kOperatorCode, "`~` is not supported", cur_.span, std::string(kNotAvailable));
            is_unary = true;
        }
    } else if (cur_.kind == TokenKind::Keyword) {
        if (text == "typeof" || text == "void" || text == "delete") {
            report(kOperatorCode, "`" + std::string(text) + "` is not supported", cur_.span,
                   std::string(kNotAvailable));
            is_unary = true;
        } else if (text == "await") {
            report("TESSERA_JSX_SUBSET_ASYNC", "`await` is not supported", cur_.span,
                   "use the async data primitives");
            is_unary = true;
        }
    }
    if (!is_unary) {
        return parse_postfix();
    }
    std::string op(text);
    advance(LexMode::Code);
    ExprPtr operand = parse_unary();
    if (!operand) {
        return nullptr;
    }
    std::vector<ExprPtr> operands;
    operands.push_back(std::move(operand));
    return make_operator(ExprKind::Unary, begin, std::move(op), std::move(operands));
}

ExprPtr Parser::parse_postfix() {
    ExprPtr expr = parse_primary();
    while (expr && !fatal_) {
        const std::uint32_t begin = expr->span.offset;
        std::vector<ExprPtr> operands;
        std::vector<TypePtr> type_args;
        const bool generic = at_punct("<") && expr->kind == ExprKind::Identifier &&
                             expr->text.starts_with("use") && parse_call_type_args(type_args);
        if (at_punct(".") || at_punct("?.")) {
            if (at_punct("?.")) {
                report(kOperatorCode, "`?.` is not supported", cur_.span,
                       std::string(kNotAvailable));
            }
            advance(LexMode::Code);
            // Reserved words are fine as property names: `row.type`.
            if (cur_.kind != TokenKind::Identifier && cur_.kind != TokenKind::Keyword) {
                fail_unexpected("a property name");
                return nullptr;
            }
            std::string name(cur_.text);
            advance(LexMode::Code);
            operands.push_back(std::move(expr));
            expr = make_operator(ExprKind::Member, begin, std::move(name), std::move(operands));
        } else if (generic || at_punct("(")) {
            operands.push_back(std::move(expr));
            advance(LexMode::Code);
            while (!fatal_ && !at_punct(")")) {
                ExprPtr argument = parse_expr();
                if (!argument) {
                    return nullptr;
                }
                operands.push_back(std::move(argument));
                if (!at_punct(",")) {
                    break;
                }
                advance(LexMode::Code);
            }
            if (!expect_punct(")", LexMode::Code)) {
                return nullptr;
            }
            expr = make_operator(ExprKind::Call, begin, {}, std::move(operands));
            if (expr) {
                expr->type_args = std::move(type_args);
            }
        } else if (at_punct("[")) {
            report("TESSERA_JSX_SUBSET_MEMBER", "indexing is not supported", cur_.span,
                   "use `.map` in JSX for lists");
            advance(LexMode::Code);
            ExprPtr index = parse_expr();
            if (!index || !expect_punct("]", LexMode::Code)) {
                return nullptr;
            }
            operands.push_back(std::move(expr));
            operands.push_back(std::move(index));
            expr = make_operator(ExprKind::Member, begin, "[]", std::move(operands));
        } else if (at_punct("++") || at_punct("--")) {
            report("TESSERA_JSX_SUBSET_MUTATION",
                   "`" + std::string(cur_.text) + "` is not supported", cur_.span,
                   "use `const` and state setters");
            advance(LexMode::Code);
        } else {
            break;
        }
    }
    return fatal_ ? nullptr : std::move(expr);
}

} // namespace tessera::compiler::jsx
