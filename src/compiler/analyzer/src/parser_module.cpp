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
    return module;
}

bool Parser::parse_item(ModuleItem& item) {
    if (cur_.is_keyword("export")) {
        item.exported = true;
        advance(LexMode::Code);
        if (cur_.is_keyword("default")) {
            fail("TESSERA_JSX_SUBSET_IMPORT", "`export default` is not supported", cur_.span,
                 "use a named export");
            return false;
        }
        if (!cur_.is_keyword("function") && !cur_.is_keyword("const") && !cur_.is_keyword("type")) {
            fail_unexpected("`function`, `const` or `type` after `export`");
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
    if (cur_.is_keyword("function")) {
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
    if (source.empty() || source.starts_with("/") || source.starts_with("\\") ||
        (source.size() > 1 && source[1] == ':')) {
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
    if (!at_punct("{")) {
        if (cur_.kind == TokenKind::Identifier || at_punct("*")) {
            fail("TESSERA_JSX_SUBSET_IMPORT", "default and namespace imports are not supported",
                 cur_.span, "use named imports: `import { Name } from \"...\";`");
        } else {
            fail_unexpected("`{` or a string after `import`");
        }
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
    item.init = parse_expr();
    return item.init != nullptr && expect_semicolon();
}

} // namespace tessera::compiler::jsx
