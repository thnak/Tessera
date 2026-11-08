// SPDX-License-Identifier: MIT

#include "tessera/ui/style/tokenizer.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

using tessera::ui::style::Token;
using tessera::ui::style::Tokenizer;
using tessera::ui::style::TokenType;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "tokenizer: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-9) {
    return std::abs(value - expected) <= tolerance;
}

const Token* find_type(const std::vector<Token>& tokens, TokenType type) {
    for (const Token& token : tokens) {
        if (token.type == type) {
            return &token;
        }
    }
    return nullptr;
}

std::size_t count_type(const std::vector<Token>& tokens, TokenType type) {
    std::size_t count = 0;
    for (const Token& token : tokens) {
        if (token.type == type) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main() {
    constexpr std::string_view source =
        "/* c */ .card:hover > #main { color: red; width: 12.5px; opacity: 50%; }";
    auto tokens = Tokenizer(source).tokenize();

    const Token* comment = find_type(tokens, TokenType::Comment);
    const Token* hash = find_type(tokens, TokenType::Hash);
    const Token* dimension = find_type(tokens, TokenType::Dimension);
    const Token* percentage = find_type(tokens, TokenType::Percentage);
    if (!expect(comment != nullptr && comment->text == "/* c */", "comment token") ||
        !expect(hash != nullptr && hash->value == "main", "hash value") ||
        !expect(dimension != nullptr && dimension->unit == "px", "dimension unit") ||
        !expect(dimension != nullptr && near(dimension->number, 12.5), "dimension number") ||
        !expect(percentage != nullptr && near(percentage->number, 50.0), "percentage number") ||
        !expect(tokens.back().type == TokenType::Eof, "eof token")) {
        return 1;
    }

    const Token* card = nullptr;
    for (const Token& token : tokens) {
        if (token.type == TokenType::Ident && token.text == "card") {
            card = &token;
            break;
        }
    }
    if (!expect(card != nullptr, "ident token") ||
        !expect(card->offset == source.find("card"), "source offset")) {
        return 2;
    }

    tokens = Tokenizer("@media (min-width: 900px) { .a { color: rgb(1, 2, 3); } }").tokenize();
    const Token* at_keyword = find_type(tokens, TokenType::AtKeyword);
    const Token* function = find_type(tokens, TokenType::Function);
    if (!expect(at_keyword != nullptr && at_keyword->value == "media", "at-keyword") ||
        !expect(function != nullptr && function->value == "rgb", "function") ||
        !expect(count_type(tokens, TokenType::Number) == 3, "function arguments")) {
        return 3;
    }

    tokens = Tokenizer("\"hello \\\"x\\\"\" 'bad\n").tokenize();
    const Token* string = find_type(tokens, TokenType::String);
    const Token* bad_string = find_type(tokens, TokenType::BadString);
    if (!expect(string != nullptr && string->value == "hello \\\"x\\\"", "string value") ||
        !expect(bad_string != nullptr && bad_string->value == "bad", "bad string value")) {
        return 4;
    }

    tokens = Tokenizer(".5 -1e2 +3 10em 50%").tokenize();
    const auto numbers = count_type(tokens, TokenType::Number);
    const Token* number_dimension = find_type(tokens, TokenType::Dimension);
    const Token* number_percentage = find_type(tokens, TokenType::Percentage);
    if (!expect(numbers == 3, "number variants") ||
        !expect(number_dimension != nullptr && number_dimension->unit == "em", "dimension unit") ||
        !expect(number_dimension != nullptr && near(number_dimension->number, 10.0),
                "dimension value") ||
        !expect(number_percentage != nullptr && near(number_percentage->number, 50.0),
                "percentage value")) {
        return 5;
    }

    constexpr std::string_view escaped = R"(\26 card)";
    tokens = Tokenizer(escaped).tokenize();
    if (!expect(tokens.size() == 2, "escape forms one ident") ||
        !expect(tokens[0].type == TokenType::Ident && tokens[0].text == escaped, "escape text")) {
        return 6;
    }

    tokens = Tokenizer("<!-- --> /* unterminated").tokenize();
    const Token* unterminated = find_type(tokens, TokenType::Comment);
    if (!expect(count_type(tokens, TokenType::Cdo) == 1, "cdo") ||
        !expect(count_type(tokens, TokenType::Cdc) == 1, "cdc") ||
        !expect(unterminated != nullptr && unterminated->text == "/* unterminated",
                "unterminated comment")) {
        return 7;
    }

    tokens = Tokenizer("~= |= ^= $= *= ||").tokenize();
    if (!expect(count_type(tokens, TokenType::IncludeMatch) == 1, "include match") ||
        !expect(count_type(tokens, TokenType::DashMatch) == 1, "dash match") ||
        !expect(count_type(tokens, TokenType::PrefixMatch) == 1, "prefix match") ||
        !expect(count_type(tokens, TokenType::SuffixMatch) == 1, "suffix match") ||
        !expect(count_type(tokens, TokenType::SubstringMatch) == 1, "substring match") ||
        !expect(count_type(tokens, TokenType::Column) == 1, "column")) {
        return 8;
    }

    tokens = Tokenizer("U+0-7F u+??").tokenize();
    if (!expect(count_type(tokens, TokenType::UnicodeRange) == 2, "unicode ranges")) {
        return 9;
    }

    tokens = Tokenizer("").tokenize();
    if (!expect(tokens.size() == 1 && tokens[0].type == TokenType::Eof, "empty source")) {
        return 10;
    }

    std::cout << "tokenizer: ok\n";
    return 0;
}
