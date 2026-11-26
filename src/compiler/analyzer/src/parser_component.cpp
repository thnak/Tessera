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
    //
    // Spike #659: with the experimental switch on, a `use...` function never reaches this function
    // (parse_item dispatches it to parse_hook), so the branch below stays the rejection of spec
    // section 7.
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
    // A component takes one props parameter; a helper may take several (ADR-066 item 4's reducer is
    // `reduce(state, action)`). The helper's body is not lowered, so its parameters only have to
    // parse.
    if (!at_punct(")")) {
        if (!parse_component_param(item)) {
            return false;
        }
        while (item.kind == ItemKind::Helper && at_punct(",")) {
            advance(LexMode::Code);
            if (!parse_component_param(item)) {
                return false;
            }
        }
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
    // ADR-066 item 4: a helper's parameters are recorded in order, because a reducer's `(state,
    // action)` is what its `switch` subject and its record results are keyed by. A component's one
    // parameter keeps the `props_*` fields instead, since that is what the props rules read.
    HelperParam helper_param;
    const auto record_helper_param = [&item](HelperParam& param, TypePtr type) {
        if (item.kind == ItemKind::Helper) {
            param.type = std::move(type);
            item.helper_params.push_back(std::move(param));
        }
    };
    if (cur_.kind == TokenKind::Identifier) {
        helper_param.name = std::string(cur_.text);
        helper_param.span = cur_.span;
        item.props_name = helper_param.name;
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
    TypePtr type = parse_type_expr();
    if (!type) {
        return false;
    }
    // A helper's parameters are the reducer's `(state, action)`, recorded in order; a component's
    // single parameter is its props type, whatever shape it took, because that is what the props
    // rules resolve. The two are different fields, so each takes ownership of its own node.
    if (item.kind == ItemKind::Helper) {
        record_helper_param(helper_param, std::move(type));
        return true;
    }
    item.props_type = std::move(type);
    return true;
}

bool Parser::parse_component_body(ModuleItem& item) {
    in_component_ = true;
    service_bindings_.clear();
    props_name_ = item.props_name;
    if (!expect_punct("{", LexMode::Code)) {
        return false;
    }
    // A helper's body is not lowered, so it is skipped rather than parsed: it may return a value,
    // call anything, and use forms the subset does not have. Skipping keeps the component beside it
    // compilable, which is the whole point of recognising the helper (issue #660's corpus).
    //
    // ADR-066 item 4 is the exception: a reducer is a helper whose `switch` body the lowering
    // reads, so a helper that declares a `switch` is parsed rather than skipped. The distinction is
    // made after the body is seen, because the body is what says which kind of helper it is.
    if (item.kind == ItemKind::Helper && !helper_has_switch()) {
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
    // ADR-066 item 4: a reducer is a helper whose body the lowering reads. It is not a component,
    // so it has no `return <element>`; its body is one `switch` and is parsed as a statement list.
    if (item.kind == ItemKind::Helper) {
        if (!parse_reducer_body(item)) {
            return false;
        }
        advance(LexMode::Code); // past `}`
        return true;
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
    // ADR-050 section 3.1 (issue #177): remember a `useService` binding so a later `nav.push(...)`
    // is left to the rules pass instead of being taken for an array method.
    if (statement.kind == StatementKind::Const && !statement.array_pattern &&
        statement.names.size() == 1 && statement.init && statement.init->kind == ExprKind::Call &&
        !statement.init->operands.empty() &&
        statement.init->operands[0]->kind == ExprKind::Identifier &&
        statement.init->operands[0]->text == "useService") {
        service_bindings_.insert(statement.names.front());
    }
    item.body.push_back(std::move(statement));
    return true;
}

bool Parser::parse_const_statement(Statement& statement) {
    statement.kind = StatementKind::Const;
    // Set when an object pattern renames a field; the initializer decides whether that is allowed
    // (a custom-hook call site) or refused (everywhere else).
    bool renamed = false;
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
        // An object pattern. Two cases share one parse:
        //
        //   * `const { state } = io;` (issue #660's corpus) binds each listed field of the
        //     initializer to a local of the same name. A rename (`{ state: s }`) is refused rather
        //     than guessed at, because the subset has no rename form and a silent rename would be a
        //     second naming rule.
        //   * `const { page, next: goNext } = usePagination(...)` (docs/specs/custom-hooks-v1.md
        //     section 3) DOES allow a rename, because the expansion pass is the only consumer and
        //     it never reaches the rules or lowering.
        //
        // The two are told apart by the initializer, which is parsed after the pattern, so a rename
        // is recorded here and refused below when the initializer is not a custom-hook call. Gating
        // the refusal on the switch alone would accept `const {a: c} = b` in every component, which
        // is the form the subset refuses.
        advance(LexMode::Code);
        while (!fatal_ && !at_punct("}")) {
            if (cur_.kind != TokenKind::Identifier && cur_.kind != TokenKind::Keyword) {
                fail_unexpected("a field name in the pattern");
                return false;
            }
            PatternBinding binding;
            binding.key = std::string(cur_.text);
            binding.local = binding.key;
            binding.span = cur_.span;
            advance(LexMode::Code);
            if (at_punct(":")) {
                renamed = true;
                advance(LexMode::Code);
                if (cur_.kind != TokenKind::Identifier) {
                    fail_unexpected("a local name after `:`");
                    return false;
                }
                binding.local = std::string(cur_.text);
                advance(LexMode::Code);
            }
            statement.names.push_back(binding.local);
            statement.object_pattern.push_back(std::move(binding));
            if (!at_punct(",")) {
                break;
            }
            advance(LexMode::Code);
        }
        if (statement.object_pattern.empty()) {
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
    // ADR-066 item 1: the safe forms of `useMemo` and `useCallback` (react-compat.md section 4.2)
    // are desugared here, where the wrapped expression is still a parse tree:
    //
    //   const v = useMemo(() => <PureExpr>, [deps]);   ->  const v = <PureExpr>;
    //   const f = useCallback(() => <body>, [deps]);   ->  const f = () => <body>;
    //
    // The memo's only effect in React is skipping a recomputation, which a pure inlined expression
    // does not need; the callback's only effect is a stable identity, which nothing in this model
    // observes. Desugaring in the parser rather than in the rules means every later pass sees the
    // ordinary form it already handles, so there is no second path to keep in step.
    //
    // A form outside the two shapes is left alone, so the rules report
    // `TESSERA_REACT_UNSUPPORTED_API` with the message that names the accepted spelling.
    desugar_memo_or_callback(statement);
    // A rename in an object pattern is allowed only at a custom-hook call site, where the expansion
    // pass consumes it (docs/specs/custom-hooks-v1.md section 3). Anywhere else it is the form the
    // subset refuses, so the check waits until the initializer is known.
    if (renamed) {
        const bool hook_call = custom_hooks_ && statement.init->kind == ExprKind::Call &&
                               !statement.init->operands.empty() &&
                               statement.init->operands[0]->kind == ExprKind::Identifier &&
                               is_hook_declaration_name(statement.init->operands[0]->text);
        if (!hook_call) {
            fail("TESSERA_JSX_SUBSET_STATEMENT", "a renamed destructured binding is not supported",
                 statement.span, "write `const <field> = <init>.<field>;` for each field");
            return false;
        }
    }
    expect_semicolon();
    return true;
}

// ADR-066 item 1: the safe forms of `useMemo` and `useCallback` (react-compat.md section 4.2).
//
//   const v = useMemo(() => <PureExpr>, [deps]);   ->  const v = <PureExpr>;
//   const f = useCallback(() => <body>, [deps]);   ->  const f = () => <body>;
//
// The memo's only effect in React is skipping a recomputation, which a pure inlined expression does
// not need; the callback's only effect is a stable identity, which nothing in this model observes.
// Desugaring here rather than in the rules means every later pass sees the ordinary form it already
// handles, so there is no second path to keep in step.
//
// The dependency list is dropped here but recorded on the statement (issue #829), so the rules pass
// can compare it with the inferred reactive set. An absent array (`useMemo(() => x)`) is accepted,
// with or without a second argument (issue #829 decision 3); the record says whether one was
// written.
//
// A form outside the two shapes is left alone, so the rules report `TESSERA_REACT_UNSUPPORTED_API`
// with the message that names the accepted spelling.
void Parser::desugar_memo_or_callback(Statement& statement) {
    if (statement.kind != StatementKind::Const || statement.array_pattern ||
        statement.names.size() != 1 || !statement.init || statement.init->kind != ExprKind::Call) {
        return;
    }
    const Expr& call = *statement.init;
    if (call.operands.size() < 2 || call.operands.size() > 3 ||
        call.operands[0]->kind != ExprKind::Identifier) {
        return;
    }
    const std::string_view name = call.operands[0]->text;
    const bool memo = name == "useMemo";
    if (!memo && name != "useCallback") {
        return;
    }
    // The wrapped value is the first argument and must be an arrow. The second, when written, is
    // the dependency list, which the desugared form does not carry (the value is recomputed on
    // every update, which is what a pure expression costs).
    const Expr* wrapped = call.operands[1].get();
    if (wrapped == nullptr || wrapped->kind != ExprKind::Arrow) {
        return;
    }
    const bool has_array = call.operands.size() == 3;
    if (has_array && call.operands[2]->kind != ExprKind::Array) {
        return;
    }
    // A memo needs an expression body (a block body would be a function value, not a value); a
    // callback keeps whatever body it has, because the arrow IS the handler.
    if (memo && (wrapped->arrow_block_body || wrapped->operands.size() != 1)) {
        return;
    }
    // Issue #829: the list is no longer silently dropped. Record it on the statement so the rules
    // pass can compare it with the inferred reactive set; the desugar itself does not change. An
    // element that is not a plain name is recorded only as its span, and the rules report the hard
    // error (the parser has no component symbol table to name what a `[a.b]` element would mean,
    // and the rules pass owns the `useEffect` rule this one mirrors).
    statement.memo_hook = std::string(name);
    statement.memo_array = has_array;
    if (has_array) {
        statement.memo_deps_span = call.operands[2]->span;
        for (const ExprPtr& element : call.operands[2]->operands) {
            if (element == nullptr) {
                continue;
            }
            if (element->kind != ExprKind::Identifier) {
                if (statement.memo_deps_error_span.length == 0) {
                    statement.memo_deps_error_span = element->span;
                }
                continue;
            }
            statement.memo_deps.push_back({element->text, element->span});
        }
    }
    // Move the wrapped arrow out of the call and make it the statement's initializer. For a memo
    // the arrow's single expression becomes the initializer directly, so the local is inlined like
    // any other `const`.
    ExprPtr& call_init = statement.init;
    if (memo) {
        statement.init = std::move(call_init->operands[1]->operands[0]);
    } else {
        statement.init = std::move(call_init->operands[1]);
    }
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
