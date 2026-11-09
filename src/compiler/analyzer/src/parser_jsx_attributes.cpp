// SPDX-License-Identifier: MIT

// JSX attributes and their rules (docs/specs/jsx-subset-v1.md sections 10.2 and 10.3).

#include "jsx_text.hpp"
#include "parser_internal.hpp"

namespace tessera::compiler::jsx {

bool Parser::parse_attributes(JsxNode& element) {
    while (!fatal_ && !at_punct(">") && !at_punct("/>")) {
        if (cur_.kind == TokenKind::Eof) {
            fail("TESSERA_JSX_PARSE_UNCLOSED_ELEMENT",
                 "tag `<" + element.tag + "` is not closed: expected `>` or `/>`",
                 element.tag_span);
            return false;
        }
        if (!parse_attribute(element)) {
            return false;
        }
    }
    return !fatal_;
}

bool Parser::parse_attribute(JsxNode& element) {
    JsxAttribute attribute;
    const std::uint32_t begin = cur_.span.offset;
    if (at_punct("{")) {
        advance(LexMode::Code);
        if (!at_punct("...")) {
            fail_unexpected("`...` in a spread attribute");
            return false;
        }
        advance(LexMode::Code);
        attribute.kind = JsxAttrKind::Spread;
        attribute.expr = parse_expr();
        if (!attribute.expr || !expect_punct("}", LexMode::JsxTag)) {
            return false;
        }
    } else if (cur_.kind == TokenKind::Identifier) {
        attribute.name = std::string(cur_.text);
        attribute.name_span = cur_.span;
        advance(LexMode::JsxTag);
        if (attribute.name == "dangerouslySetInnerHTML") {
            report("TESSERA_JSX_SUBSET_DOM", "`dangerouslySetInnerHTML` is not supported",
                   attribute.name_span, "not available");
        } else if (attribute.name.find('-') != std::string::npos &&
                   attribute.name.rfind("aria-", 0) != 0) {
            report("TESSERA_JSX_SUBSET_UNKNOWN_PROP",
                   "attribute `" + attribute.name + "` is not an identifier", attribute.name_span,
                   "only `aria-*` names may contain `-`");
        }
        while (at_punct(":")) {
            report("TESSERA_JSX_SUBSET_UNKNOWN_PROP",
                   "namespaced attribute names are not supported", cur_.span);
            advance(LexMode::JsxTag);
            if (cur_.kind == TokenKind::Identifier) {
                advance(LexMode::JsxTag);
            }
        }
        if (at_punct("=")) {
            advance(LexMode::JsxTag);
            if (!parse_attribute_value(attribute)) {
                return false;
            }
        }
    } else {
        fail_unexpected("an attribute, `>` or `/>`");
        return false;
    }
    attribute.span = span_from(begin);
    check_attribute_rules(attribute);
    element.attributes.push_back(std::move(attribute));
    return true;
}

bool Parser::parse_attribute_value(JsxAttribute& attribute) {
    if (cur_.kind == TokenKind::String) {
        std::vector<EntityError> errors;
        attribute.kind = JsxAttrKind::String;
        attribute.string_value = decode_entities(cur_.value, &errors);
        for (const auto& error : errors) {
            report("TESSERA_JSX_PARSE_UNKNOWN_ENTITY", error.message, cur_.span);
        }
        advance(LexMode::JsxTag);
        return true;
    }
    if (at_punct("{")) {
        advance(LexMode::Code);
        if (at_punct("}")) {
            fail("TESSERA_JSX_PARSE_EMPTY_EXPRESSION", "the attribute expression is empty",
                 cur_.span);
            return false;
        }
        attribute.kind = JsxAttrKind::Expression;
        attribute.expr = parse_expr();
        return attribute.expr != nullptr && expect_punct("}", LexMode::JsxTag);
    }
    fail_unexpected("an attribute value: a string or `{expression}`");
    return false;
}

} // namespace tessera::compiler::jsx
