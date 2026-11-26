// SPDX-License-Identifier: MIT

// Module items and per-item recovery (docs/specs/jsx-subset-v1.md sections 4 and 14).

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {
namespace {

bool is_item_keyword(const Token& token) {
    return token.kind == TokenKind::Keyword &&
           (token.text == "import" || token.text == "export" || token.text == "type" ||
            token.text == "function" || token.text == "const");
}

bool is_line_break(char c) {
    return c == '\n' || c == '\r';
}

// A component name starts uppercase (docs/specs/jsx-subset-v1.md section 6). Used to decide whether
// a `const` is worth trying as an arrow component, so a lowercase helper never pays for the
// attempt.
bool is_component_name(std::string_view name) {
    return !name.empty() && name[0] >= 'A' && name[0] <= 'Z';
}

// Absolute, drive-letter and UNC specifiers are malformed for every import form (spec section 4).
bool is_absolute_source(const std::string& source) {
    return source.starts_with("/") || source.starts_with("\\") ||
           (source.size() > 1 && source[1] == ':');
}

} // namespace

// A token that can start a top-level item at the start of its line. Spec section 14 asks for
// brace depth 0; a keyword in column 1 is also accepted, so one unclosed `{` does not swallow
// every later item.
bool Parser::at_item_start() const {
    if (!is_item_keyword(cur_)) {
        return false;
    }
    const std::string_view source = map_.source();
    std::size_t index = cur_.span.offset;
    const bool column_one = index == 0 || is_line_break(source[index - 1]);
    while (index > 0 && (source[index - 1] == ' ' || source[index - 1] == '\t')) {
        --index;
    }
    const bool line_start = index == 0 || is_line_break(source[index - 1]);
    return line_start && (brace_depth_ == 0 || column_one);
}

void Parser::recover_to_next_item(std::uint32_t item_start) {
    // Tokens skipped here were never meant to be valid; their lexer diagnostics are noise.
    const auto mark = sink_.mark();
    while (cur_.kind != TokenKind::Eof && !(at_item_start() && cur_.span.offset > item_start)) {
        advance(LexMode::Code);
    }
    if (!lexer_.stopped()) {
        sink_.rewind(mark);
    }
}

std::unique_ptr<Module> Parser::parse_module_root() {
    if (lexer_.stopped()) {
        return nullptr; // the file size limit was reported by the lexer
    }
    auto module = std::make_unique<Module>();
    advance(LexMode::Code);
    std::size_t components = 0;
    while (cur_.kind != TokenKind::Eof) {
        brace_depth_ = 0;
        in_component_ = false;
        ModuleItem item;
        const std::uint32_t start = cur_.span.offset;
        const std::size_t errors_before = sink_.error_count();
        parse_item(item);
        item.span = Span{start, prev_end_ > start ? prev_end_ - start : 0};
        check_item_comments(item, sink_.error_count() == errors_before);
        item.valid = sink_.error_count() == errors_before;
        if (item.kind == ItemKind::Component && ++components > limits_.max_components) {
            report("TESSERA_JSX_LIMIT_COMPONENTS",
                   "file has more than " + std::to_string(limits_.max_components) + " components",
                   item.span, "split the file");
            break;
        }
        module->items.push_back(std::move(item));
        if (fatal_) {
            fatal_ = false;
            if (lexer_.stopped()) {
                break;
            }
            recover_to_next_item(start);
            skip_comments_before_current();
        }
    }
    check_remaining_comments();
    expand_type_forms(*module);
    return module;
}

bool Parser::parse_item(ModuleItem& item) {
    if (cur_.is_keyword("export")) {
        item.exported = true;
        advance(LexMode::Code);
        if (cur_.is_keyword("default")) {
            // ADR-066 item 1: `export default function Name(...)` is accepted, because a page-level
            // component is written that way in most React apps and the name is still declared. The
            // default-ness is dropped: the subset has no default exports, so the component is
            // reachable by its own name like any other. An anonymous default export stays rejected,
            // since a component with no name has no id.
            advance(LexMode::Code);
            if (!cur_.is_keyword("function")) {
                fail("TESSERA_JSX_SUBSET_IMPORT", "`export default` is not supported for this form",
                     cur_.span, "use `export default function Name(...)` or a named export");
                return false;
            }
            item.default_export = true;
            return parse_component(item);
        }
        if (!cur_.is_keyword("function") && !cur_.is_keyword("const") && !cur_.is_keyword("type") &&
            !cur_.is_keyword("interface")) {
            fail_unexpected("`function`, `const`, `type` or `interface` after `export`");
            return false;
        }
    }
    if (cur_.is_keyword("import") && !item.exported) {
        return parse_import(item);
    }
    if (cur_.is_keyword("type")) {
        if (item.exported) {
            // `export type X = ...` is a type alias; `export type { A } [from "..."]` re-exports
            // type names (spec section 4). One token of lookahead decides which.
            const Checkpoint saved = checkpoint();
            advance(LexMode::Code);
            const bool re_export = at_punct("{");
            restore(saved);
            if (re_export) {
                return parse_reexport(item);
            }
        }
        return parse_type_alias(item);
    }
    if (cur_.is_keyword("interface")) {
        return parse_interface(item);
    }
    if (cur_.is_keyword("function")) {
        // Spike #659: with the experimental switch on, a `use...` function is a hook declaration
        // rather than the `TESSERA_REACT_CUSTOM_HOOK` rejection. The name is one token ahead of
        // `function`, so the decision needs a lookahead rather than a second parse.
        if (custom_hooks_) {
            const Checkpoint saved = checkpoint();
            advance(LexMode::Code);
            const bool hook =
                cur_.kind == TokenKind::Identifier && is_hook_declaration_name(cur_.text);
            restore(saved);
            if (hook) {
                return parse_hook(item);
            }
        }
        return parse_component(item);
    }
    if (cur_.is_keyword("const")) {
        return parse_const_item(item);
    }
    if (cur_.kind == TokenKind::Keyword) {
        parse_rejected_keyword();
    } else {
        fail_unexpected("an import, type, function or const declaration");
    }
    return false;
}

// The specifier rules shared by named imports and type re-exports (spec section 4): a relative
// path or a package specifier; empty, absolute and drive/UNC paths are malformed.
void Parser::check_module_source(ModuleItem& item, Span span) {
    const std::string& source = item.source;
    if (item.side_effect) {
        if (!source.ends_with(".css")) {
            report("TESSERA_JSX_SUBSET_IMPORT", "a side-effect import must name a `.css` file",
                   span, "use `import \"./styles.css\";`");
        }
        return;
    }
    if (!item.css_module_binding.empty()) {
        // A CSS-module default import names a stylesheet rather than a module, so the package
        // rules do not apply to it; the path rules still do (issue #463).
        if (is_absolute_source(source)) {
            report("TESSERA_JSX_SUBSET_IMPORT",
                   "an import source must be a package name or a relative path", span,
                   "use `from \"./Card.module.css\"`");
        }
        return;
    }
    if (source.empty() || is_absolute_source(source)) {
        report("TESSERA_JSX_SUBSET_IMPORT",
               "an import source must be a package name or a relative path", span,
               "use `from \"tessera\"`, `from \"pkg/Name\"` or `from \"./Name\"`");
    }
}

// `{ A, B as C, }`; an empty list is accepted (`import {} from "x"`).
bool Parser::parse_import_names(ModuleItem& item) {
    if (!expect_punct("{", LexMode::Code)) {
        return false;
    }
    while (!fatal_ && !at_punct("}")) {
        if (cur_.kind != TokenKind::Identifier) {
            fail_unexpected("an imported name");
            return false;
        }
        ImportName name;
        name.name = std::string(cur_.text);
        name.span = cur_.span;
        advance(LexMode::Code);
        if (cur_.is_keyword("as")) {
            advance(LexMode::Code);
            if (cur_.kind != TokenKind::Identifier) {
                fail_unexpected("a local name after `as`");
                return false;
            }
            name.local_name = std::string(cur_.text);
            advance(LexMode::Code);
        }
        item.names.push_back(std::move(name));
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    return expect_punct("}", LexMode::Code);
}

bool Parser::parse_import(ModuleItem& item) {
    item.kind = ItemKind::Import;
    advance(LexMode::Code);
    if (cur_.is_keyword("type")) {
        // `import type { T } from "./types";`: a type-only binding has no runtime import edge.
        item.type_only = true;
        advance(LexMode::Code);
        if (cur_.kind == TokenKind::String) {
            fail_unexpected("`{` after `import type`");
            return false;
        }
    }
    if (cur_.kind == TokenKind::String) {
        item.side_effect = true;
        item.source = cur_.value;
        check_module_source(item, cur_.span);
        advance(LexMode::Code);
        return expect_semicolon();
    }
    if (cur_.kind == TokenKind::Identifier) {
        // The one default import the subset accepts (issue #463): `import styles from
        // "./Card.module.css";` binds a CSS module's class map. The grammar is unambiguous here —
        // `{` starts named imports and `*` a namespace import — so an identifier directly after
        // `import` can only be a default binding, and no lookahead is needed.
        const std::string binding(cur_.text);
        const Span binding_span = cur_.span;
        advance(LexMode::Code);
        if (!cur_.is_keyword("from")) {
            fail_unexpected("`from`");
            return false;
        }
        advance(LexMode::Code);
        if (cur_.kind != TokenKind::String) {
            fail_unexpected("a module path string");
            return false;
        }
        item.source = cur_.value;
        // `import type styles from ...` binds a type, and a class map is not one, so the
        // type-only form keeps the generic rejection.
        if (!item.type_only && item.source.ends_with(".module.css")) {
            item.css_module_binding = binding;
            check_module_source(item, cur_.span);
            advance(LexMode::Code);
            return expect_semicolon();
        }
        // ADR-066 item 1: `import React from "react"` is accepted and ignored. The automatic JSX
        // runtime means the binding is never read, so a port that keeps the import compiles; any
        // other default import stays the rejection below. The binding is recorded as ignored so the
        // lowering does not look for a value it will never find.
        if (!item.type_only && item.source == "react" && binding == "React") {
            item.ignored_react_import = true;
            check_module_source(item, cur_.span);
            advance(LexMode::Code);
            return expect_semicolon();
        }
        fail("TESSERA_JSX_SUBSET_IMPORT", "default and namespace imports are not supported",
             binding_span, "use named imports: `import { Name } from \"...\";`");
        return false;
    }
    if (at_punct("*")) {
        // ADR-066 item 1: `import * as React from "react"` is the namespace spelling of the same
        // unused import, so it is accepted and ignored too. Any other namespace import stays the
        // rejection.
        const Span star_span = cur_.span;
        const Checkpoint saved = checkpoint();
        advance(LexMode::Code); // past `*`
        if (cur_.is_keyword("as")) {
            advance(LexMode::Code);
            if (cur_.kind == TokenKind::Identifier && cur_.text == "React") {
                advance(LexMode::Code);
                if (cur_.is_keyword("from")) {
                    advance(LexMode::Code);
                    if (cur_.kind == TokenKind::String && cur_.value == "react") {
                        item.source = cur_.value;
                        item.ignored_react_import = true;
                        check_module_source(item, cur_.span);
                        advance(LexMode::Code);
                        return expect_semicolon();
                    }
                }
            }
        }
        restore(std::move(saved));
        fail("TESSERA_JSX_SUBSET_IMPORT", "default and namespace imports are not supported",
             star_span, "use named imports: `import { Name } from \"...\";`");
        return false;
    }
    if (!at_punct("{")) {
        fail_unexpected("`{` or a string after `import`");
        return false;
    }
    if (!parse_import_names(item)) {
        return false;
    }
    if (!cur_.is_keyword("from")) {
        fail_unexpected("`from`");
        return false;
    }
    advance(LexMode::Code);
    if (cur_.kind != TokenKind::String) {
        fail_unexpected("a module path string");
        return false;
    }
    item.source = cur_.value;
    check_module_source(item, cur_.span);
    advance(LexMode::Code);
    return expect_semicolon();
}

bool Parser::parse_reexport(ModuleItem& item) {
    item.kind = ItemKind::ReExport;
    item.type_only = true;
    advance(LexMode::Code); // `type`
    if (!parse_import_names(item)) {
        return false;
    }
    if (cur_.is_keyword("from")) {
        advance(LexMode::Code);
        if (cur_.kind != TokenKind::String) {
            fail_unexpected("a module path string");
            return false;
        }
        item.source = cur_.value;
        check_module_source(item, cur_.span);
        advance(LexMode::Code);
    }
    return expect_semicolon();
}

bool Parser::parse_type_alias(ModuleItem& item) {
    item.kind = ItemKind::TypeAlias;
    advance(LexMode::Code);
    if (cur_.kind != TokenKind::Identifier) {
        fail_unexpected("a type name");
        return false;
    }
    item.name = std::string(cur_.text);
    item.name_span = cur_.span;
    advance(LexMode::Code);
    if (!expect_punct("=", LexMode::Code)) {
        return false;
    }
    item.type = parse_type_expr();
    return item.type != nullptr && expect_semicolon();
}

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

bool Parser::parse_const_item(ModuleItem& item) {
    item.kind = ItemKind::Const;
    advance(LexMode::Code);
    if (cur_.kind != TokenKind::Identifier) {
        fail_unexpected("a constant name");
        return false;
    }
    item.name = std::string(cur_.text);
    item.name_span = cur_.span;
    advance(LexMode::Code);
    if (at_punct(":")) {
        advance(LexMode::Code);
        item.type = parse_type_expr();
        if (!item.type) {
            return false;
        }
    }
    if (!expect_punct("=", LexMode::Code)) {
        return false;
    }
    // ADR-066 item 1: `const Avatar = ({ initials }: Props) => { ... return <div/>; };` is an
    // arrow-function component, the most common spelling in ported code. It is tried first and
    // rewound when it is not one, because the two forms share the `const Name = (` prefix: a
    // component needs a props annotation and a JSX return, and an ordinary arrow has neither.
    // Only an uppercase name is a candidate, so `const format = (v: string) => v;` never pays for
    // the attempt.
    if (is_component_name(item.name) && (at_punct("(") || at_punct("{"))) {
        const Checkpoint saved = checkpoint();
        const std::string name = item.name;
        const Span name_span = item.name_span;
        const bool exported = item.exported;
        if (parse_arrow_component(item)) {
            return true;
        }
        // Not an arrow component: rewind every token, diagnostic and flag the attempt produced and
        // parse the same text as an ordinary constant.
        fatal_ = false;
        restore(std::move(saved));
        item = ModuleItem{};
        item.kind = ItemKind::Const;
        item.name = name;
        item.name_span = name_span;
        item.exported = exported;
        if (!expect_punct("=", LexMode::Code)) {
            return false;
        }
    }
    item.init = parse_expr();
    return item.init != nullptr && expect_semicolon();
}

// `({ initials, size = 32 }: Props) => { ... }` or `() => { ... }`, with the body of a component
// (docs/specs/jsx-subset-v1.md section 6). cur_ is `(` or `{`.
bool Parser::parse_arrow_component(ModuleItem& item) {
    item.kind = ItemKind::Component;
    if (!expect_punct("(", LexMode::Code)) {
        return false;
    }
    if (!at_punct(")") && !parse_component_param(item)) {
        return false;
    }
    if (!expect_punct(")", LexMode::Code) || !expect_punct("=>", LexMode::Code)) {
        return false;
    }
    // An expression body (`=> <div/>`) is the same component with one returned element, so it is
    // accepted too; a block body goes through the ordinary component rules.
    if (at_punct("<")) {
        item.returned = parse_jsx_element(LexMode::Code);
        if (!item.returned) {
            return false;
        }
        expect_semicolon();
        return true;
    }
    if (!parse_component_body(item)) {
        return false;
    }
    // `const Avatar = (...) => { ... };` ends with a `;` that a `function` declaration does not
    // have. It is optional here so both spellings are accepted.
    if (at_punct(";")) {
        advance(LexMode::Code);
    }
    return true;
}

} // namespace tessera::compiler::jsx
