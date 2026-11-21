// SPDX-License-Identifier: MIT

// Component functions (docs/specs/jsx-subset-v1.md sections 6 and 7). Hooks are parsed as the plain
// statements they are made of; the analyzer decides which calls are hooks.

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {
namespace {

// A prop default is a literal, a flat list of literals (`validators = []`), or an object literal
// for a prop whose type is a declared record (`{ padding = { top: 4 } }`, ADR-051). The IR keeps
// the literal value; anything computed would need a runtime and is rejected at parse time.
//
// An object literal is accepted structurally: `parser_literals.cpp` already rejects a spread and a
// computed key, and whether the *field values* are literals is checked by the lowering, which knows
// the record's members and can name the offending field (`TESSERA_IR_UNSUPPORTED`). Checking it
// here too would only move that message to a generic parse error with no member name.
bool is_literal_default(const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Number:
    case ExprKind::String:
    case ExprKind::Boolean:
    case ExprKind::Null:
        return true;
    case ExprKind::Object:
        return true;
    case ExprKind::Array:
        for (const ExprPtr& element : expr.operands) {
            if (!element || element->kind == ExprKind::Array || !is_literal_default(*element)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

} // namespace

bool Parser::parse_component(ModuleItem& item) {
    item.kind = ItemKind::Component;
    advance(LexMode::Code); // past `function`
    if (cur_.kind != TokenKind::Identifier) {
        fail_unexpected("a component name");
        return false;
    }
    item.name = std::string(cur_.text);
    item.name_span = cur_.span;
    // A lowercase `function` is a module-scope helper, not a badly named component (issue #660's
    // corpus: 16 files were rejected for this, including 13 of the repository's own React sources).
    // The distinction is the name, not the body: the subset's rule is that a component name starts
    // uppercase, so a lowercase declaration is simply not a component and is parsed as a helper.
    // A helper is not lowered -- the port moves it into a native handler or a `const` arrow -- but
    // it must not stop the component beside it from being compiled.
    const bool lowercase = !item.name.empty() && (item.name[0] < 'A' || item.name[0] > 'Z');
    if (item.name.starts_with("use")) {
        report("TESSERA_REACT_CUSTOM_HOOK",
               "custom hook `" + excerpt(item.name) + "` is not supported", cur_.span,
               "inline the hook or write a native handler");
    } else if (lowercase) {
        item.kind = ItemKind::Helper;
    }
    advance(LexMode::Code);
    if (!expect_punct("(", LexMode::Code)) {
        return false;
    }
    if (!at_punct(")") && !parse_component_param(item)) {
        return false;
    }
    if (!expect_punct(")", LexMode::Code)) {
        return false;
    }
    if (at_punct(":")) {
        advance(LexMode::Code);
        item.type = parse_type_expr();
        if (!item.type) {
            return false;
        }
    }
    return parse_component_body(item);
}

bool Parser::parse_component_param(ModuleItem& item) {
    item.has_props = true;
    if (cur_.kind == TokenKind::Identifier) {
        item.props_name = std::string(cur_.text);
        advance(LexMode::Code);
    } else if (at_punct("{")) {
        item.props_destructured = true;
        advance(LexMode::Code);
        while (!fatal_ && !at_punct("}")) {
            if (cur_.kind != TokenKind::Identifier) {
                fail_unexpected("a prop name");
                return false;
            }
            PropBinding binding;
            binding.name = std::string(cur_.text);
            binding.span = cur_.span;
            advance(LexMode::Code);
            if (at_punct("=")) {
                advance(LexMode::Code);
                binding.default_value = parse_expr();
                if (!binding.default_value) {
                    return false;
                }
                if (!is_literal_default(*binding.default_value)) {
                    report("TESSERA_JSX_SUBSET_LITERAL",
                           "a prop default must be a literal or a literal list",
                           binding.default_value->span,
                           "use a number, string, boolean, null or a list of those");
                }
            }
            item.props_bindings.push_back(std::move(binding));
            if (!at_punct(",")) {
                break;
            }
            advance(LexMode::Code);
        }
        if (!expect_punct("}", LexMode::Code)) {
            return false;
        }
    } else {
        fail_unexpected("a props name or `{`");
        return false;
    }
    // The annotation is required when the parameter exists (spec section 6).
    if (!expect_punct(":", LexMode::Code)) {
        return false;
    }
    item.props_type = parse_type_expr();
    return item.props_type != nullptr;
}

bool Parser::parse_component_body(ModuleItem& item) {
    in_component_ = true;
    props_name_ = item.props_name;
    if (!expect_punct("{", LexMode::Code)) {
        return false;
    }
    // A helper's body is not lowered, so it is skipped rather than parsed: it may return a value,
    // call anything, and use forms the subset does not have. Skipping keeps the component beside it
    // compilable, which is the whole point of recognising the helper (issue #660's corpus).
    if (item.kind == ItemKind::Helper) {
        int depth = 1;
        while (!fatal_ && depth > 0) {
            if (cur_.kind == TokenKind::Eof) {
                fail_unexpected("`}`");
                return false;
            }
            if (at_punct("{")) {
                ++depth;
            } else if (at_punct("}")) {
                --depth;
            }
            advance(LexMode::Code);
        }
        return !fatal_;
    }
    while (!fatal_) {
        if (cur_.is_keyword("return")) {
            if (!parse_returned_element(item)) {
                return false;
            }
            break;
        }
        if (at_punct("}")) {
            fail("TESSERA_JSX_PARSE_MISSING_RETURN",
                 "component `" + excerpt(item.name) + "` must end with `return <element>;`",
                 cur_.span);
            return false;
        }
        if (!parse_statement(item)) {
            return false;
        }
    }
    if (fatal_) {
        return false;
    }
    if (!at_punct("}")) {
        fail_unexpected("`}`: `return` must be the last statement of a component");
        return false;
    }
    advance(LexMode::Code);
    return true;
}

bool Parser::parse_statement(ModuleItem& item) {
    Statement statement;
    const std::uint32_t begin = cur_.span.offset;
    if (cur_.is_keyword("const")) {
        if (!parse_const_statement(statement)) {
            return false;
        }
    } else {
        statement.kind = StatementKind::Expression;
        statement.init = parse_expr();
        if (!statement.init) {
            return false;
        }
        expect_semicolon();
    }
    statement.span = span_from(begin);
    item.body.push_back(std::move(statement));
    return true;
}

bool Parser::parse_const_statement(Statement& statement) {
    statement.kind = StatementKind::Const;
    advance(LexMode::Code); // past `const`
    if (at_punct("[")) {
        statement.array_pattern = true;
        advance(LexMode::Code);
        while (!fatal_ && cur_.kind == TokenKind::Identifier) {
            statement.names.emplace_back(cur_.text);
            advance(LexMode::Code);
            if (!at_punct(",")) {
                break;
            }
            advance(LexMode::Code);
        }
        if (statement.names.empty()) {
            fail_unexpected("a name in the pattern");
            return false;
        }
        if (!expect_punct("]", LexMode::Code)) {
            return false;
        }
    } else if (at_punct("{")) {
        // `const { state } = io;` (issue #660's corpus). Each listed name binds the field of the
        // same name; a rename (`{ state: s }`) is refused rather than guessed at, because the
        // subset has no rename form and a silent rename would be a second naming rule.
        statement.object_pattern = true;
        advance(LexMode::Code);
        while (!fatal_ && !at_punct("}")) {
            if (cur_.kind != TokenKind::Identifier && cur_.kind != TokenKind::Keyword) {
                fail_unexpected("a field name in the pattern");
                return false;
            }
            statement.names.emplace_back(cur_.text);
            advance(LexMode::Code);
            if (at_punct(":")) {
                fail("TESSERA_JSX_SUBSET_STATEMENT",
                     "a renamed destructured binding is not supported", cur_.span,
                     "write `const <field> = <init>.<field>;` for each field");
                return false;
            }
            if (!at_punct(",")) {
                break;
            }
            advance(LexMode::Code);
        }
        if (statement.names.empty()) {
            fail_unexpected("a field name in the pattern");
            return false;
        }
        if (!expect_punct("}", LexMode::Code)) {
            return false;
        }
    } else if (cur_.kind == TokenKind::Identifier) {
        statement.names.emplace_back(cur_.text);
        advance(LexMode::Code);
    } else {
        fail_unexpected("a name or `[`");
        return false;
    }
    if (at_punct(":")) {
        advance(LexMode::Code);
        statement.type = parse_type_expr();
        if (!statement.type) {
            return false;
        }
    }
    if (!expect_punct("=", LexMode::Code)) {
        return false;
    }
    statement.init = parse_expr();
    if (!statement.init) {
        return false;
    }
    expect_semicolon();
    return true;
}

// `return <A/>;` or `return (<A/>);`. Anything but one JSX element is an error (spec section 6).
bool Parser::parse_returned_element(ModuleItem& item) {
    advance(LexMode::Code); // past `return`
    const bool parenthesized = at_punct("(");
    if (parenthesized) {
        advance(LexMode::Code);
    }
    if (!at_punct("<")) {
        fail_unexpected("a JSX element after `return`");
        return false;
    }
    item.returned = parse_jsx_element(LexMode::Code);
    if (!item.returned) {
        return false;
    }
    if (parenthesized && !expect_punct(")", LexMode::Code)) {
        return false;
    }
    expect_semicolon();
    return true;
}

} // namespace tessera::compiler::jsx
