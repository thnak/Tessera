// SPDX-License-Identifier: MIT

// Template, array and object literals (docs/specs/jsx-subset-v1.md sections 3.4, 8.4, 8.5).

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {

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
    ExprPtr spread_base;
    while (!fatal_ && !at_punct("}")) {
        // ADR-066 item 3: `{ ...record, field: value }` updates a record value with known fields
        // (ADR-051). The base must be the first entry and appear once, so the written members are
        // unambiguous; anything else keeps the old rejection.
        if (at_punct("...")) {
            if (spread_base || !properties.empty()) {
                fail("TESSERA_JSX_SUBSET_LITERAL",
                     "a record spread must be the first entry of the object literal", cur_.span,
                     "write `{ ...state, field: value }`");
                return nullptr;
            }
            advance(LexMode::Code);
            spread_base = parse_expr();
            if (!spread_base) {
                return nullptr;
            }
            if (!at_punct(",") && !at_punct("}")) {
                fail_unexpected("`,` or `}`");
                return nullptr;
            }
            if (at_punct(",")) {
                advance(LexMode::Code);
            }
            continue;
        }
        ObjectProperty property;
        property.key_span = cur_.span;
        if (cur_.kind == TokenKind::Identifier || cur_.kind == TokenKind::Keyword) {
            property.key = std::string(cur_.text);
        } else if (cur_.kind == TokenKind::String) {
            property.key = std::move(cur_.value);
            property.key_is_string = true;
        } else if (at_punct("[")) {
            fail("TESSERA_JSX_SUBSET_LITERAL", "computed object keys are not supported", cur_.span,
                 "use literal keys");
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
    expr->spread_base = std::move(spread_base);
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

} // namespace tessera::compiler::jsx
