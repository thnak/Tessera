// SPDX-License-Identifier: MIT

// Custom hook declarations (spike #659, ADR-065; docs/specs/custom-hooks-v1.md section 2).
//
// A hook is a function declaration whose name is `use` plus an uppercase letter. It is parsed only
// when the experimental `FrontEndOptions::custom_hooks` switch is on; otherwise `parse_component`
// keeps the `TESSERA_REACT_CUSTOM_HOOK` rejection and the item is a component, so the default front
// end is byte-identical to the one before this file existed.
//
// The body has the shape of a component body (hook calls, handler declarations, pure `const` locals
// and exactly one `return`), but the return is a value rather than JSX. The parser records the
// shape; the expansion pass and the rules decide what the values may be.

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {
namespace {

// A parameter default is a literal (spec section 2). A computed default would need a run time the
// expansion does not have, so it is rejected here rather than at the call site.
bool is_literal_default(const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Number:
    case ExprKind::String:
    case ExprKind::Boolean:
    case ExprKind::Null:
        return true;
    default:
        return false;
    }
}

} // namespace

bool is_hook_declaration_name(std::string_view name) {
    return name.size() > 3 && name.starts_with("use") && name[3] >= 'A' && name[3] <= 'Z';
}

bool Parser::parse_hook(ModuleItem& item) {
    item.kind = ItemKind::Hook;
    advance(LexMode::Code); // past `function`
    if (cur_.kind != TokenKind::Identifier) {
        fail_unexpected("a hook name");
        return false;
    }
    item.name = std::string(cur_.text);
    item.name_span = cur_.span;
    if (!is_hook_declaration_name(item.name)) {
        report("TESSERA_REACT_HOOK_FORM",
               "`" + excerpt(item.name) +
                   "` is not a hook name; a custom hook is `use` plus an uppercase letter",
               cur_.span, "rename it, for example `usePagination`");
    }
    advance(LexMode::Code);
    if (!expect_punct("(", LexMode::Code)) {
        return false;
    }
    if (!at_punct(")") && !parse_hook_params(item)) {
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
    return parse_hook_body(item);
}

// `(total: int, pageSize: int = 20)`: positional, typed, with an optional literal default. No
// destructuring, no rest, no `this` (spec section 2).
bool Parser::parse_hook_params(ModuleItem& item) {
    while (!fatal_ && !at_punct(")")) {
        if (cur_.kind != TokenKind::Identifier) {
            fail_unexpected("a parameter name");
            return false;
        }
        CustomHookParam param;
        param.name = std::string(cur_.text);
        param.span = cur_.span;
        advance(LexMode::Code);
        if (!expect_punct(":", LexMode::Code)) {
            return false;
        }
        param.type = parse_type_expr();
        if (!param.type) {
            return false;
        }
        if (at_punct("=")) {
            advance(LexMode::Code);
            param.default_value = parse_expr();
            if (!param.default_value) {
                return false;
            }
            if (!is_literal_default(*param.default_value)) {
                report("TESSERA_REACT_HOOK_FORM", "a hook parameter default must be a literal",
                       param.default_value->span,
                       "use a number, string, boolean or null, or pass the value at the call site");
            }
        }
        item.hook_params.push_back(std::move(param));
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    return true;
}

// The statements of the body, then exactly one `return <value>;` which is last (spec section 2).
bool Parser::parse_hook_body(ModuleItem& item) {
    in_component_ = true; // the props-spread rule does not apply, but the flag keeps recovery sane
    if (!expect_punct("{", LexMode::Code)) {
        return false;
    }
    while (!fatal_) {
        if (cur_.is_keyword("return")) {
            if (!parse_hook_return(item)) {
                return false;
            }
            break;
        }
        if (at_punct("}")) {
            fail("TESSERA_REACT_HOOK_RETURN",
                 "hook `" + excerpt(item.name) + "` must end with `return <value>;`", cur_.span,
                 "return one value, an array literal or an object literal");
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
        fail_unexpected("`}`: `return` must be the last statement of a hook");
        return false;
    }
    advance(LexMode::Code);
    return true;
}

// `return <value>;`, optionally parenthesized. The value is a pure expression, an array literal or
// an object literal; JSX is `TESSERA_REACT_HOOK_RETURN` (spec section 2).
bool Parser::parse_hook_return(ModuleItem& item) {
    advance(LexMode::Code); // past `return`
    const bool parenthesized = at_punct("(");
    if (parenthesized) {
        advance(LexMode::Code);
    }
    if (at_punct("<")) {
        fail("TESSERA_REACT_HOOK_RETURN", "a hook returns a value, not JSX", cur_.span,
             "return a pure expression, an array literal or an object literal");
        return false;
    }
    item.hook_return = parse_expr();
    if (!item.hook_return) {
        return false;
    }
    if (parenthesized && !expect_punct(")", LexMode::Code)) {
        return false;
    }
    expect_semicolon();
    return true;
}

} // namespace tessera::compiler::jsx
