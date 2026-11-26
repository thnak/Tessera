// SPDX-License-Identifier: MIT

// ADR-058 typing rule 1 (#547): the declaration forms a port writes that the subset absorbs without
// an edit - `interface`, string `enum` and `as const` objects - and the `Color.Red` read of the
// namespaces they declare. Split from parser_module.cpp, which holds the module item dispatch.

#include "parser_internal.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::jsx {

// ADR-058 typing rule 1 (#547): `interface X { ... }` is a `type X = { ... }`. A port writes
// interfaces for props and records, and the two are the same thing once there is no declaration
// merging, `extends` or generics, so the form is absorbed without an edit. The forms that would
// need those are rejected with the fix instead of being half-lowered.
bool Parser::parse_interface(ModuleItem& item) {
    item.kind = ItemKind::TypeAlias;
    advance(LexMode::Code); // `interface`
    if (cur_.kind != TokenKind::Identifier) {
        fail_unexpected("an interface name");
        return false;
    }
    item.name = std::string(cur_.text);
    item.name_span = cur_.span;
    advance(LexMode::Code);
    if (!at_punct("{")) {
        const bool extends = cur_.text == "extends";
        fail("TESSERA_JSX_SUBSET_TYPES",
             extends ? "interface `extends` is not supported"
                     : "a generic interface is not supported",
             cur_.span,
             extends ? "write the inherited members out, or use one `type` with all the members"
                     : "declare one interface per concrete type");
        return false;
    }
    item.type = parse_type_object(cur_.span.offset);
    return item.type != nullptr;
}

namespace {

TypePtr make_enum_type(TypeKind kind, Span span) {
    auto type = std::make_unique<TypeNode>();
    type->kind = kind;
    type->span = span;
    return type;
}

} // namespace

// `enum Color { Red = "red", Blue = "blue" }` is `type Color = "red" | "blue"` plus the namespace
// `Color.Red` reads from. Only string members are accepted: a numeric or auto-numbered enum has no
// string-literal union to stand for it, so it is rejected with the fix.
bool Parser::parse_enum(ModuleItem& item) {
    item.kind = ItemKind::TypeAlias;
    const std::uint32_t begin = cur_.span.offset;
    advance(LexMode::Code); // `enum`
    if (cur_.kind != TokenKind::Identifier) {
        fail_unexpected("an enum name");
        return false;
    }
    item.name = std::string(cur_.text);
    item.name_span = cur_.span;
    advance(LexMode::Code);
    if (!expect_punct("{", LexMode::Code)) {
        return false;
    }
    std::vector<NamespaceMember> members;
    TypePtr uni = make_enum_type(TypeKind::Union, Span{});
    while (!fatal_ && !at_punct("}")) {
        if (cur_.kind != TokenKind::Identifier && cur_.kind != TokenKind::Keyword) {
            fail_unexpected("an enum member name");
            return false;
        }
        const std::string member = std::string(cur_.text);
        const Span member_span = cur_.span;
        advance(LexMode::Code);
        if (!at_punct("=")) {
            fail("TESSERA_JSX_TYPE_ENUM", "enum member `" + member + "` has no string value",
                 member_span, "write `" + member + " = \"" + member + "\"`");
            return false;
        }
        advance(LexMode::Code);
        if (cur_.kind != TokenKind::String) {
            fail("TESSERA_JSX_TYPE_ENUM", "enum member `" + member + "` must be a string literal",
                 cur_.span, "use a string enum, or a union of string literals");
            return false;
        }
        TypePtr literal = make_enum_type(TypeKind::StringLiteral, cur_.span);
        literal->text = cur_.value;
        members.push_back(NamespaceMember{member, ExprKind::String, cur_.value, 0.0});
        uni->children.push_back(std::move(literal));
        advance(LexMode::Code);
        if (at_punct(",")) {
            advance(LexMode::Code);
        } else if (!at_punct("}")) {
            fail_unexpected("`,` or `}`");
            return false;
        }
    }
    if (!expect_punct("}", LexMode::Code)) {
        return false;
    }
    uni->span = span_from(begin);
    if (uni->children.size() == 1) {
        item.type = std::move(uni->children[0]);
    } else if (uni->children.empty()) {
        fail("TESSERA_JSX_TYPE_ENUM", "an enum needs at least one member", item.name_span,
             "declare the members, or remove the enum");
        return false;
    } else {
        item.type = std::move(uni);
    }
    value_namespaces_[item.name] = std::move(members);
    return true;
}

// `as const` asserts literal types, which the subset infers anyway, so it is accepted and dropped.
// A constant whose value is an object of string or number literals also becomes a namespace, so
// `COLORS.red` reads the literal (the `as const` object stand-in for an enum).
bool Parser::finish_const_object(ModuleItem& item) {
    if (cur_.is_keyword("as")) {
        advance(LexMode::Code);
        if (!cur_.is_keyword("const")) {
            fail("TESSERA_JSX_SUBSET_TYPES", "only `as const` is supported as an assertion",
                 cur_.span, "remove the assertion, or declare the type on the constant");
            return false;
        }
        advance(LexMode::Code);
    }
    if (item.init && item.init->kind == ExprKind::Object && item.init->spread_base == nullptr) {
        std::vector<NamespaceMember> members;
        for (const ObjectProperty& property : item.init->properties) {
            const Expr* value = property.value.get();
            if (value == nullptr ||
                (value->kind != ExprKind::String && value->kind != ExprKind::Number)) {
                return true; // not a literal table: an ordinary object constant
            }
            members.push_back(
                NamespaceMember{property.key, value->kind, value->text, value->number});
        }
        value_namespaces_[item.name] = std::move(members);
    }
    return true;
}

ExprPtr Parser::resolve_namespace_member(ExprPtr object, std::string name, std::uint32_t begin) {
    if (object->kind != ExprKind::Identifier) {
        return object;
    }
    const auto space = value_namespaces_.find(object->text);
    if (space == value_namespaces_.end()) {
        return object;
    }
    for (const NamespaceMember& member : space->second) {
        if (member.name == name) {
            ExprPtr literal = make_expr(member.kind, begin);
            literal->text = member.text;
            literal->number = member.number;
            return literal;
        }
    }
    std::string known;
    for (const NamespaceMember& member : space->second) {
        known += (known.empty() ? "" : ", ") + member.name;
    }
    report("TESSERA_JSX_TYPE_ENUM", "`" + object->text + "` has no member `" + name + "`",
           span_from(begin), "the members are: " + known);
    // A placeholder keeps the parse going, so one wrong member is one diagnostic and not a cascade.
    return make_expr(ExprKind::String, begin);
}

} // namespace tessera::compiler::jsx
