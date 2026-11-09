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

bool Parser::parse_import(ModuleItem& item) {
    item.kind = ItemKind::Import;
    advance(LexMode::Code);
    const auto check_source = [&] {
        const std::string& source = item.source;
        const bool relative = source.starts_with("./") || source.starts_with("../");
        if (item.side_effect ? !source.ends_with(".css") : (source != "tessera" && !relative)) {
            report("TESSERA_JSX_SUBSET_IMPORT",
                   item.side_effect ? "a side-effect import must name a `.css` file"
                                    : "imports come from \"tessera\" or a relative path",
                   cur_.span, "use `from \"tessera\"` or `from \"./Name\"`");
        }
    };
    if (cur_.kind == TokenKind::String) {
        item.side_effect = true;
        item.source = cur_.value;
        check_source();
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
    advance(LexMode::Code);
    while (!fatal_ && !at_punct("}")) {
        if (cur_.kind != TokenKind::Identifier) {
            fail_unexpected("an imported name");
            return false;
        }
        item.names.push_back(ImportName{std::string(cur_.text), cur_.span});
        advance(LexMode::Code);
        if (cur_.is_keyword("as")) {
            fail("TESSERA_JSX_SUBSET_IMPORT", "renaming imports is not supported", cur_.span,
                 "import the name as it is exported");
            return false;
        }
        if (!at_punct(",")) {
            break;
        }
        advance(LexMode::Code);
    }
    if (!expect_punct("}", LexMode::Code)) {
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
    check_source();
    advance(LexMode::Code);
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
