// SPDX-License-Identifier: MIT

#pragma once

#include "scan.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string_view>

// Token classification shared by the code-layout translation units.
namespace tessera::compiler::format::code {

inline constexpr std::size_t kNone = static_cast<std::size_t>(-1);

inline bool in_set(std::string_view text, std::initializer_list<std::string_view> set) {
    return std::find(set.begin(), set.end(), text) != set.end();
}

inline bool is_literal_word(const FTok& tok) {
    return in_set(tok.text, {"true", "false", "null", "this"});
}

inline bool is_binary(std::string_view text) {
    return in_set(text,
                  {"=",  "==", "!=", "===", "!==", "+",  "-",  "*",  "/",  "%",  "&&", "||", "??",
                   "=>", "&",  "|",  "<=",  ">=",  "**", "+=", "-=", "*=", "/=", "%=", "^"});
}

// Whether the tokens up to and including `tok` end an operand, which separates a unary `-` from a
// binary one and a call's `(` from a keyword's.
inline bool word_operand(const FTok& tok) {
    return tok.kind == FKind::Word && (!tok.keyword || is_literal_word(tok));
}

inline bool statement_keyword(std::string_view text) {
    return in_set(text, {"const", "return", "if", "for", "while", "switch", "function", "export",
                         "import", "type", "case", "default", "break", "continue", "throw", "try"});
}

inline bool continues_statement(const FTok& tok) {
    if (tok.kind == FKind::Word) {
        return in_set(tok.text,
                      {"else", "catch", "finally", "while", "as", "in", "of", "instanceof"});
    }
    return tok.kind != FKind::Eof;
}

inline bool ends_value(const FTok& tok) {
    switch (tok.kind) {
    case FKind::Number:
    case FKind::String:
    case FKind::Atom:
        return true;
    case FKind::Word:
        return word_operand(tok);
    case FKind::Punct:
        return tok.text == ")" || tok.text == "]" || tok.text == "}" || tok.text == ">";
    default:
        return false;
    }
}

} // namespace tessera::compiler::format::code
