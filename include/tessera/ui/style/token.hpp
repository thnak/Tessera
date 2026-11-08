// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::style {

enum class TokenType : std::uint8_t {
    Eof,
    Whitespace,
    Comment,
    Ident,
    Function,
    AtKeyword,
    Hash,
    String,
    BadString,
    Number,
    Percentage,
    Dimension,
    Delim,
    Colon,
    Semicolon,
    Comma,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    LeftBrace,
    RightBrace,
    Cdo,
    Cdc,
    IncludeMatch,
    DashMatch,
    PrefixMatch,
    SuffixMatch,
    SubstringMatch,
    Column,
    UnicodeRange,
};

struct Token {
    TokenType type = TokenType::Eof;
    std::size_t offset = 0;
    std::string_view text;
    std::string_view value;
    double number = 0.0;
    std::string_view unit;
};

} // namespace tessera::ui::style
