// SPDX-License-Identifier: MIT

// Type expressions (docs/specs/jsx-subset-v1.md section 5).

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {
namespace {

TypePtr make_type(TypeKind kind, Span span) {
    auto type = std::make_unique<TypeNode>();
    type->kind = kind;
    type->span = span;
    return type;
}

} // namespace

bool Parser::expect_type_close() {
    if (fatal_) {
        return false;
    }
    if (at_punct(">")) {
        advance(LexMode::Code);
        return true;
    }
    if (cur_.kind == TokenKind::Punct && cur_.text.starts_with('>')) {
        // `Array<Array<int>>` lexes `>>` as one token: consume only its first character.
        const std::uint32_t after_first = cur_.span.offset + 1;
        lexer_.seek(after_first);
        prev_end_ = after_first;
        cur_ = lexer_.next(LexMode::Code);
        return true;
    }
    fail_unexpected("`>`");
    return false;
}

TypePtr Parser::parse_type_expr() {
    const DepthScope scope(*this);
    if (!scope.entered()) {
        return nullptr;
    }
    const std::uint32_t begin = cur_.span.offset;
    TypePtr first = parse_type_primary();
    if (!first || !at_punct("|")) {
        return fatal_ ? nullptr : std::move(first);
    }
    TypePtr uni = make_type(TypeKind::Union, Span{});
    uni->children.push_back(std::move(first));
    while (at_punct("|")) {
        advance(LexMode::Code);
        TypePtr next = parse_type_primary();
        if (!next) {
            return nullptr;
        }
        uni->children.push_back(std::move(next));
    }
    uni->span = span_from(begin);
    return finish_node(*uni) ? std::move(uni) : nullptr;
}

TypePtr Parser::parse_type_primary() {
    const std::uint32_t begin = cur_.span.offset;
    TypePtr type;
    if (cur_.kind == TokenKind::Identifier) {
        const std::string name(cur_.text);
        const Span name_span = cur_.span;
        advance(LexMode::Code);
        type = make_type(TypeKind::Name, name_span);
        type->text = name;
        if (name == "any" || name == "unknown" || name == "never" || name == "object" ||
            name == "Function") {
            report("TESSERA_JSX_SUBSET_TYPES", "type `" + excerpt(name) + "` is not supported",
                   name_span, "use the types of the subset specification, section 5");
        } else if (name == "number") {
            report("TESSERA_JSX_TYPE_NUMBER", "bare `number` is not a Tessera type", name_span,
                   "write `int` or `float`");
        }
        if (at_punct("<")) {
            advance(LexMode::Code);
            while (!fatal_) {
                TypePtr argument = parse_type_expr();
                if (!argument) {
                    return nullptr;
                }
                type->children.push_back(std::move(argument));
                if (!at_punct(",")) {
                    break;
                }
                advance(LexMode::Code);
            }
            if (!expect_type_close()) {
                return nullptr;
            }
            type->span = span_from(begin);
        }
    } else if (cur_.is_keyword("null") || cur_.is_keyword("void")) {
        // `void` is not in the section 5 grammar but section 9.3 writes `): void` for handlers.
        const std::string name(cur_.text);
        advance(LexMode::Code);
        type = make_type(TypeKind::Name, span_from(begin));
        type->text = name;
    } else if (cur_.kind == TokenKind::String) {
        std::string value = std::move(cur_.value);
        advance(LexMode::Code);
        type = make_type(TypeKind::StringLiteral, span_from(begin));
        type->text = std::move(value);
    } else if (at_punct("{")) {
        type = parse_type_object(begin);
    } else if (at_punct("(")) {
        type = parse_type_function(begin);
    } else if (at_punct("[")) {
        fail("TESSERA_JSX_SUBSET_TYPES", "tuple types are not supported", cur_.span,
             "use an array type such as `int[]`");
        return nullptr;
    } else if (cur_.is_keyword("typeof") || cur_.is_keyword("interface") ||
               cur_.is_keyword("enum")) {
        fail("TESSERA_JSX_SUBSET_TYPES", "`" + std::string(cur_.text) + "` types are not supported",
             cur_.span, "use the types of the subset specification, section 5");
        return nullptr;
    } else {
        fail_unexpected("a type");
        return nullptr;
    }
    if (!type) {
        return nullptr;
    }
    while (!fatal_ && at_punct("[")) {
        advance(LexMode::Code);
        if (!expect_punct("]", LexMode::Code)) {
            return nullptr;
        }
        TypePtr array = make_type(TypeKind::Array, span_from(begin));
        array->children.push_back(std::move(type));
        if (!finish_node(*array)) {
            return nullptr;
        }
        type = std::move(array);
    }
    if (at_punct("&")) {
        fail("TESSERA_JSX_SUBSET_TYPES", "intersection types are not supported", cur_.span,
             "use an object type with all the members");
        return nullptr;
    }
    return fatal_ ? nullptr : std::move(type);
}

TypePtr Parser::parse_type_object(std::uint32_t begin) {
    advance(LexMode::Code); // past `{`
    std::vector<TypeMember> members;
    while (!fatal_ && !at_punct("}")) {
        if (cur_.kind != TokenKind::Identifier && cur_.kind != TokenKind::Keyword) {
            fail_unexpected("a member name");
            return nullptr;
        }
        TypeMember member;
        member.name = std::string(cur_.text);
        const std::uint32_t member_begin = cur_.span.offset;
        advance(LexMode::Code);
        if (at_punct("?")) {
            member.optional = true;
            advance(LexMode::Code);
        }
        if (!expect_punct(":", LexMode::Code)) {
            return nullptr;
        }
        member.type = parse_type_expr();
        if (!member.type) {
            return nullptr;
        }
        member.span = span_from(member_begin);
        members.push_back(std::move(member));
        if (at_punct(";") || at_punct(",")) {
            advance(LexMode::Code);
        } else {
            break;
        }
    }
    if (!expect_punct("}", LexMode::Code)) {
        return nullptr;
    }
    TypePtr type = make_type(TypeKind::Object, span_from(begin));
    type->members = std::move(members);
    return finish_node(*type) ? std::move(type) : nullptr;
}

TypePtr Parser::parse_type_function(std::uint32_t begin) {
    advance(LexMode::Code); // past `(`
    TypePtr type = make_type(TypeKind::Function, Span{});
    while (!fatal_ && !at_punct(")")) {
        if (cur_.kind != TokenKind::Identifier) {
            fail_unexpected("a parameter name");
            return nullptr;
        }
        type->param_names.emplace_back(cur_.text);
        advance(LexMode::Code);
        if (!expect_punct(":", LexMode::Code)) {
            return nullptr;
        }
        TypePtr parameter = parse_type_expr();
        if (!parameter) {
            return nullptr;
        }
        type->children.push_back(std::move(parameter));
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    if (!expect_punct(")", LexMode::Code) || !expect_punct("=>", LexMode::Code)) {
        return nullptr;
    }
    TypePtr result = parse_type_expr();
    if (!result) {
        return nullptr;
    }
    type->children.push_back(std::move(result));
    type->span = span_from(begin);
    return finish_node(*type) ? std::move(type) : nullptr;
}

// `useState<int>(0)`: after the callee, `<` may open type arguments. Only checked when the
// result is followed by `(`; otherwise this was a comparison and everything is rewound.
bool Parser::parse_call_type_args(std::vector<TypePtr>& type_args) {
    Checkpoint saved = checkpoint();
    advance(LexMode::Code); // past `<`
    std::vector<TypePtr> parsed;
    bool ok = true;
    while (!fatal_) {
        TypePtr argument = parse_type_expr();
        if (!argument) {
            ok = false;
            break;
        }
        parsed.push_back(std::move(argument));
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    ok = ok && !fatal_ && expect_type_close() && at_punct("(");
    if (!ok) {
        fatal_ = false;
        restore(std::move(saved));
        return false;
    }
    type_args = std::move(parsed);
    return true;
}

} // namespace tessera::compiler::jsx
