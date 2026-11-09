// SPDX-License-Identifier: MIT

// Primary expressions (docs/specs/jsx-subset-v1.md section 8.1) and the arrow forms of section 9.

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {

ExprPtr Parser::parse_primary() {
    const std::uint32_t begin = cur_.span.offset;
    switch (cur_.kind) {
    case TokenKind::Number: {
        const std::string text(cur_.text);
        const double number = cur_.number;
        advance(LexMode::Code);
        ExprPtr expr = make_expr(ExprKind::Number, begin);
        expr->text = text;
        expr->number = number;
        return expr;
    }
    case TokenKind::String: {
        std::string value = std::move(cur_.value);
        advance(LexMode::Code);
        ExprPtr expr = make_expr(ExprKind::String, begin);
        expr->text = std::move(value);
        return expr;
    }
    case TokenKind::TemplateString: {
        std::string value = std::move(cur_.value);
        advance(LexMode::Code);
        ExprPtr expr = make_expr(ExprKind::Template, begin);
        expr->template_strings.push_back(std::move(value));
        return expr;
    }
    case TokenKind::TemplateHead:
        return parse_template();
    case TokenKind::Identifier: {
        std::string name(cur_.text);
        advance(LexMode::Code);
        if (at_punct("=>")) {
            ArrowSignature signature;
            signature.params.push_back(std::move(name));
            signature.types.emplace_back();
            return parse_arrow(begin, std::move(signature));
        }
        ExprPtr expr = make_expr(ExprKind::Identifier, begin);
        expr->text = std::move(name);
        return expr;
    }
    case TokenKind::Keyword: {
        const std::string_view word = cur_.text;
        if (word == "true" || word == "false" || word == "null") {
            advance(LexMode::Code);
            ExprPtr expr = make_expr(word == "null" ? ExprKind::Null : ExprKind::Boolean, begin);
            expr->boolean = word == "true";
            return expr;
        }
        return parse_rejected_keyword();
    }
    case TokenKind::Punct:
        if (at_punct("(")) {
            return parse_parenthesized_or_arrow();
        }
        if (at_punct("[")) {
            return parse_array();
        }
        if (at_punct("{")) {
            return parse_object();
        }
        if (at_punct("<")) {
            return parse_jsx_expression();
        }
        break;
    default:
        break;
    }
    fail_unexpected("an expression");
    return nullptr;
}

ExprPtr Parser::parse_rejected_keyword() {
    const std::string word(cur_.text);
    std::string_view code;
    std::string_view suggestion;
    if (word == "class" || word == "extends" || word == "new" || word == "this" ||
        word == "super") {
        code = "TESSERA_JSX_SUBSET_CLASS";
        suggestion = "use function components";
    } else if (word == "let" || word == "var") {
        code = "TESSERA_JSX_SUBSET_MUTATION";
        suggestion = "use `const` and state setters";
    } else if (word == "async" || word == "await" || word == "yield") {
        code = "TESSERA_JSX_SUBSET_ASYNC";
        suggestion = "use the async data primitives";
    } else if (word == "import" || word == "export") {
        code = "TESSERA_JSX_SUBSET_IMPORT";
        suggestion = "use named imports";
    } else if (word == "function") {
        code = "TESSERA_JSX_SUBSET_FUNCTION_VALUE";
        suggestion = "declare a handler";
    } else if (word == "typeof" || word == "void" || word == "delete" || word == "in" ||
               word == "instanceof") {
        code = "TESSERA_JSX_SUBSET_OPERATOR";
        suggestion = "not available in V1";
    } else if (word == "enum" || word == "interface") {
        code = "TESSERA_JSX_SUBSET_TYPES";
        suggestion = "use the types of the subset specification, section 5";
    } else if (word != "type" && word != "as" && word != "from") {
        code = "TESSERA_JSX_SUBSET_STATEMENT";
        suggestion = "use `?:`, `&&` and `.map` inside JSX";
    }
    if (code.empty()) {
        fail_unexpected("an expression");
    } else {
        fail(code, "`" + word + "` is not supported", cur_.span, std::string(suggestion));
    }
    return nullptr;
}

// `return e` / `return` inside a block body. Which blocks may contain one (an effect cleanup, a
// preview body) is decided by the analyzer; the parser only records it.
ExprPtr Parser::parse_return_statement() {
    const std::uint32_t begin = cur_.span.offset;
    advance(LexMode::Code);
    std::vector<ExprPtr> operands;
    if (!at_punct(";") && !at_punct("}")) {
        ExprPtr value = parse_expr();
        if (!value) {
            return nullptr;
        }
        operands.push_back(std::move(value));
    }
    ExprPtr expr = make_expr(ExprKind::Return, begin);
    expr->operands = std::move(operands);
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

ExprPtr Parser::parse_parenthesized_or_arrow() {
    const std::uint32_t begin = cur_.span.offset;
    // `(a, b) =>` and `(expr)` share a prefix, so try the arrow parameter list first and back
    // out (tokens, comments and diagnostics) when it is not one.
    Checkpoint saved = checkpoint();
    ArrowSignature signature;
    advance(LexMode::Code);
    bool params_ok = true;
    while (!at_punct(")")) {
        if (cur_.kind != TokenKind::Identifier) {
            params_ok = false;
            break;
        }
        signature.params.emplace_back(cur_.text);
        advance(LexMode::Code);
        TypePtr type;
        if (at_punct(":")) { // typed parameter of a native handler (spec 9.3)
            advance(LexMode::Code);
            type = parse_type_expr();
            if (!type) {
                params_ok = false;
                break;
            }
        }
        signature.types.push_back(std::move(type));
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    if (params_ok && at_punct(")")) {
        advance(LexMode::Code);
        // `(a: T): R =>`. In the then-branch of `c ? (a) : b => d` the colon belongs to the
        // conditional, so a return type is not tried there.
        if (at_punct(":") && conditional_then_depth_ == 0) {
            advance(LexMode::Code);
            signature.return_type = parse_type_expr();
            params_ok = signature.return_type != nullptr;
        }
        if (params_ok && at_punct("=>")) {
            return parse_arrow(begin, std::move(signature));
        }
    }
    fatal_ = false; // an error inside the attempt is re-found, or not, by the real parse
    restore(std::move(saved));
    advance(LexMode::Code);
    ExprPtr inner = parse_expr();
    if (!inner) {
        return nullptr;
    }
    if (!at_punct(")")) {
        fail_unexpected("`)`");
        return nullptr;
    }
    advance(LexMode::Code);
    inner->span = span_from(begin); // the node covers its parentheses
    return inner;
}

ExprPtr Parser::parse_arrow(std::uint32_t begin, ArrowSignature signature) {
    advance(LexMode::Code); // past `=>`
    std::vector<ExprPtr> body;
    bool block = false;
    if (at_punct("{")) {
        block = true;
        advance(LexMode::Code);
        while (!fatal_ && !at_punct("}")) {
            ExprPtr statement = cur_.is_keyword("return") ? parse_return_statement() : parse_expr();
            if (!statement) {
                return nullptr;
            }
            body.push_back(std::move(statement));
            if (at_punct(";")) {
                advance(LexMode::Code);
            } else if (!at_punct("}")) {
                fail_unexpected("`;` or `}`");
                return nullptr;
            }
        }
        if (!expect_punct("}", LexMode::Code)) {
            return nullptr;
        }
    } else {
        ExprPtr value = parse_expr();
        if (!value) {
            return nullptr;
        }
        body.push_back(std::move(value));
    }
    ExprPtr expr = make_expr(ExprKind::Arrow, begin);
    expr->params = std::move(signature.params);
    expr->param_types = std::move(signature.types);
    expr->return_type = std::move(signature.return_type);
    expr->operands = std::move(body);
    expr->arrow_block_body = block;
    return finish_node(*expr) ? std::move(expr) : nullptr;
}
} // namespace tessera::compiler::jsx
