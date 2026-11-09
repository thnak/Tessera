// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/source_map.hpp"

#include <string>
#include <string_view>

namespace tessera::compiler::jsx {

enum class TokenKind {
    Eof,
    Identifier,
    Keyword, // reserved words of spec section 3.3 (code mode only)
    Number,
    String,         // value = decoded contents
    TemplateString, // `...` without substitutions; value = cooked text
    TemplateHead,   // `...${   value = cooked text before the substitution
    TemplateMiddle, // }...${
    TemplateTail,   // }...`
    Punct,          // `text` is the punctuator, including operators the subset rejects
    JsxText,        // raw text between tags; `text` is the unprocessed source slice
    Unknown,        // a character the language does not use; a diagnostic was reported
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    Span span;
    std::string_view text; // exact source slice
    std::string value;     // decoded payload for String and Template* tokens
    double number = 0.0;   // value of Number tokens

    [[nodiscard]] bool is_punct(std::string_view punctuator) const {
        return kind == TokenKind::Punct && text == punctuator;
    }
    [[nodiscard]] bool is_keyword(std::string_view word) const {
        return kind == TokenKind::Keyword && text == word;
    }
};

// JSX needs a lexer driven by the parser because the same characters mean different things in
// an expression, inside a tag, and between tags.
enum class LexMode {
    Code,     // expressions and statements
    JsxTag,   // after `<`: names may contain `-`, `>` and `/>` are single tokens, no keywords
    JsxChild, // between `>` and the next `<` or `{`: raw text
};

struct Comment {
    Span span;             // whole comment including delimiters
    bool block = false;    // `/* */` when true, `//` otherwise
    std::string_view text; // contents without delimiters
};

} // namespace tessera::compiler::jsx
