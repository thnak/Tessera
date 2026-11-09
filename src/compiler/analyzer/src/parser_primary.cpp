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
            std::vector<std::string> params;
            params.push_back(std::move(name));
            return parse_arrow(begin, std::move(params));
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

ExprPtr Parser::parse_parenthesized_or_arrow() {
    const std::uint32_t begin = cur_.span.offset;
    // `(a, b) =>` and `(expr)` share a prefix, so try the arrow parameter list first and back
    // out (tokens, comments and diagnostics) when it is not one.
    Checkpoint saved = checkpoint();
    std::vector<std::string> params;
    advance(LexMode::Code);
    bool params_ok = true;
    while (!at_punct(")")) {
        if (cur_.kind != TokenKind::Identifier) {
            params_ok = false;
            break;
        }
        params.emplace_back(cur_.text);
        advance(LexMode::Code);
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    if (params_ok && at_punct(")")) {
        advance(LexMode::Code);
        if (at_punct("=>")) {
            return parse_arrow(begin, std::move(params));
        }
    }
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

ExprPtr Parser::parse_arrow(std::uint32_t begin, std::vector<std::string> params) {
    advance(LexMode::Code); // past `=>`
    std::vector<ExprPtr> body;
    bool block = false;
    if (at_punct("{")) {
        block = true;
        advance(LexMode::Code);
        while (!fatal_ && !at_punct("}")) {
            ExprPtr statement = parse_expr();
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
    expr->params = std::move(params);
    expr->operands = std::move(body);
    expr->arrow_block_body = block;
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

ExprPtr Parser::parse_template() {
    const std::uint32_t begin = cur_.span.offset;
    std::vector<std::string> strings;
    std::vector<ExprPtr> substitutions;
    strings.push_back(std::move(cur_.value));
    while (true) {
        advance(LexMode::Code);
        ExprPtr substitution = parse_expr();
        if (!substitution) {
            return nullptr;
        }
        substitutions.push_back(std::move(substitution));
        if (!at_punct("}")) {
            fail_unexpected("`}` to close the template substitution");
            return nullptr;
        }
        advance_template();
        if (cur_.kind != TokenKind::TemplateMiddle && cur_.kind != TokenKind::TemplateTail) {
            fail_unexpected("the rest of the template literal");
            return nullptr;
        }
        const bool last = cur_.kind == TokenKind::TemplateTail;
        strings.push_back(std::move(cur_.value));
        if (last) {
            break;
        }
    }
    advance(LexMode::Code);
    ExprPtr expr = make_expr(ExprKind::Template, begin);
    expr->template_strings = std::move(strings);
    expr->operands = std::move(substitutions);
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

ExprPtr Parser::parse_array() {
    const std::uint32_t begin = cur_.span.offset;
    advance(LexMode::Code);
    std::vector<ExprPtr> elements;
    while (!fatal_ && !at_punct("]")) {
        ExprPtr element = parse_expr();
        if (!element) {
            return nullptr;
        }
        elements.push_back(std::move(element));
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    if (!expect_punct("]", LexMode::Code)) {
        return nullptr;
    }
    ExprPtr expr = make_expr(ExprKind::Array, begin);
    expr->operands = std::move(elements);
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

ExprPtr Parser::parse_object() {
    const std::uint32_t begin = cur_.span.offset;
    advance(LexMode::Code);
    std::vector<ObjectProperty> properties;
    while (!fatal_ && !at_punct("}")) {
        ObjectProperty property;
        property.key_span = cur_.span;
        if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword) {
            property.key = std::string(cur_.text);
        } else if (cur_.kind == TokenKind::String) {
            property.key = std::move(cur_.value);
            property.key_is_string = true;
        } else if (at_punct("[") || at_punct("...")) {
            fail("TESSERA_JSX_SUBSET_LITERAL",
                 at_punct("[") ? "computed object keys are not supported"
                               : "spread in an object literal is not supported",
                 cur_.span, "use literal keys");
            return nullptr;
        } else {
            fail_unexpected("a property name");
            return nullptr;
        }
        advance(LexMode::Code);
        if (at_punct(":")) {
            advance(LexMode::Code);
            property.value = parse_expr();
        } else if (!property.key_is_string && (at_punct(",") || at_punct("}"))) {
            property.value = make_expr(ExprKind::Identifier, property.key_span.offset);
            property.value->span = property.key_span;
            property.value->text = property.key;
        } else {
            fail_unexpected("`:`");
        }
        if (!property.value) {
            return nullptr;
        }
        properties.push_back(std::move(property));
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    if (!expect_punct("}", LexMode::Code)) {
        return nullptr;
    }
    ExprPtr expr = make_expr(ExprKind::Object, begin);
    expr->properties = std::move(properties);
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

} // namespace tessera::compiler::jsx
