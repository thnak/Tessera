// SPDX-License-Identifier: MIT

#include "tessera/ui/style/tokenizer.hpp"

#include <cstdlib>
#include <string>

namespace tessera::ui::style {
namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_name_start(char c) {
    const auto value = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || value >= 0x80;
}

bool is_name_char(char c) {
    return is_name_start(c) || is_digit(c) || c == '-';
}

bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_newline(char c) {
    return c == '\n' || c == '\r' || c == '\f';
}

bool is_valid_escape(std::string_view source, std::size_t position) {
    return position < source.size() && source[position] == '\\' && position + 1 < source.size() &&
           !is_newline(source[position + 1]);
}

} // namespace

Tokenizer::Tokenizer(std::string_view source)
    : source_(source) {}

Token Tokenizer::next() {
    if (at_end()) {
        return Token{TokenType::Eof, position_, {}, {}, 0.0, {}};
    }

    const std::size_t start = position_;
    const char c = source_[position_];

    if (is_whitespace(c)) {
        while (!at_end() && is_whitespace(source_[position_])) {
            ++position_;
        }
        const std::string_view text = source_.substr(start, position_ - start);
        return Token{TokenType::Whitespace, start, text, text, 0.0, {}};
    }

    if (matches(position_, "/*")) {
        position_ += 2;
        while (!at_end() && !matches(position_, "*/")) {
            ++position_;
        }
        if (matches(position_, "*/")) {
            position_ += 2;
        }
        const std::string_view text = source_.substr(start, position_ - start);
        return Token{TokenType::Comment, start, text, text, 0.0, {}};
    }

    if (matches(position_, "<!--")) {
        position_ += 4;
        const std::string_view text = source_.substr(start, 4);
        return Token{TokenType::Cdo, start, text, text, 0.0, {}};
    }

    if (matches(position_, "-->")) {
        position_ += 3;
        const std::string_view text = source_.substr(start, 3);
        return Token{TokenType::Cdc, start, text, text, 0.0, {}};
    }

    if (c == '"' || c == '\'') {
        const char quote = c;
        ++position_;
        const std::size_t value_start = position_;
        bool bad = false;
        while (!at_end()) {
            const char current = source_[position_];
            if (current == quote) {
                break;
            }
            if (current == '\\') {
                consume_escape();
                continue;
            }
            if (is_newline(current)) {
                bad = true;
                break;
            }
            ++position_;
        }
        const std::size_t value_end = position_;
        if (!bad && !at_end() && source_[position_] == quote) {
            ++position_;
        }
        const std::string_view text = source_.substr(start, position_ - start);
        const std::string_view value = source_.substr(value_start, value_end - value_start);
        return Token{bad ? TokenType::BadString : TokenType::String, start, text, value, 0.0, {}};
    }

    if (c == '#' && starts_name(position_ + 1)) {
        ++position_;
        const std::size_t value_start = position_;
        consume_name();
        const std::string_view text = source_.substr(start, position_ - start);
        const std::string_view value = source_.substr(value_start, position_ - value_start);
        return Token{TokenType::Hash, start, text, value, 0.0, {}};
    }

    if (c == '@' && starts_ident(position_ + 1)) {
        ++position_;
        const std::size_t value_start = position_;
        consume_name();
        const std::string_view text = source_.substr(start, position_ - start);
        const std::string_view value = source_.substr(value_start, position_ - value_start);
        return Token{TokenType::AtKeyword, start, text, value, 0.0, {}};
    }

    if (starts_unicode_range(position_)) {
        consume_unicode_range();
        const std::string_view text = source_.substr(start, position_ - start);
        return Token{TokenType::UnicodeRange, start, text, text, 0.0, {}};
    }

    if (starts_number(position_)) {
        consume_number();
        const std::size_t number_end = position_;
        if (!at_end() && source_[position_] == '%') {
            ++position_;
            const std::string_view text = source_.substr(start, position_ - start);
            const std::string number_text = std::string(source_.substr(start, number_end - start));
            return Token{TokenType::Percentage,
                         start,
                         text,
                         text,
                         std::strtod(number_text.c_str(), nullptr),
                         {}};
        }
        if (starts_ident(position_)) {
            const std::size_t unit_start = position_;
            consume_name();
            const std::string_view text = source_.substr(start, position_ - start);
            const std::string number_text = std::string(source_.substr(start, number_end - start));
            return Token{TokenType::Dimension,
                         start,
                         text,
                         source_.substr(unit_start, position_ - unit_start),
                         std::strtod(number_text.c_str(), nullptr),
                         source_.substr(unit_start, position_ - unit_start)};
        }
        const std::string_view text = source_.substr(start, position_ - start);
        const std::string number_text(text);
        return Token{
            TokenType::Number, start, text, text, std::strtod(number_text.c_str(), nullptr), {}};
    }

    if (starts_ident(position_)) {
        consume_name();
        const std::size_t name_end = position_;
        if (!at_end() && source_[position_] == '(') {
            ++position_;
            const std::string_view text = source_.substr(start, position_ - start);
            return Token{
                TokenType::Function, start, text, source_.substr(start, name_end - start), 0.0, {}};
        }
        const std::string_view text = source_.substr(start, position_ - start);
        return Token{TokenType::Ident, start, text, text, 0.0, {}};
    }

    if (matches(position_, "~=")) {
        position_ += 2;
        const std::string_view text = source_.substr(start, 2);
        return Token{TokenType::IncludeMatch, start, text, text, 0.0, {}};
    }
    if (matches(position_, "|=")) {
        position_ += 2;
        const std::string_view text = source_.substr(start, 2);
        return Token{TokenType::DashMatch, start, text, text, 0.0, {}};
    }
    if (matches(position_, "^=")) {
        position_ += 2;
        const std::string_view text = source_.substr(start, 2);
        return Token{TokenType::PrefixMatch, start, text, text, 0.0, {}};
    }
    if (matches(position_, "$=")) {
        position_ += 2;
        const std::string_view text = source_.substr(start, 2);
        return Token{TokenType::SuffixMatch, start, text, text, 0.0, {}};
    }
    if (matches(position_, "*=")) {
        position_ += 2;
        const std::string_view text = source_.substr(start, 2);
        return Token{TokenType::SubstringMatch, start, text, text, 0.0, {}};
    }
    if (matches(position_, "||")) {
        position_ += 2;
        const std::string_view text = source_.substr(start, 2);
        return Token{TokenType::Column, start, text, text, 0.0, {}};
    }

    ++position_;
    const std::string_view text = source_.substr(start, 1);
    switch (c) {
    case ':':
        return Token{TokenType::Colon, start, text, text, 0.0, {}};
    case ';':
        return Token{TokenType::Semicolon, start, text, text, 0.0, {}};
    case ',':
        return Token{TokenType::Comma, start, text, text, 0.0, {}};
    case '(':
        return Token{TokenType::LeftParen, start, text, text, 0.0, {}};
    case ')':
        return Token{TokenType::RightParen, start, text, text, 0.0, {}};
    case '[':
        return Token{TokenType::LeftBracket, start, text, text, 0.0, {}};
    case ']':
        return Token{TokenType::RightBracket, start, text, text, 0.0, {}};
    case '{':
        return Token{TokenType::LeftBrace, start, text, text, 0.0, {}};
    case '}':
        return Token{TokenType::RightBrace, start, text, text, 0.0, {}};
    default:
        return Token{TokenType::Delim, start, text, text, 0.0, {}};
    }
}

std::vector<Token> Tokenizer::tokenize() {
    std::vector<Token> tokens;
    for (Token token = next(); token.type != TokenType::Eof; token = next()) {
        tokens.push_back(token);
    }
    tokens.push_back(next());
    return tokens;
}

bool Tokenizer::at_end() const {
    return position_ >= source_.size();
}

bool Tokenizer::starts_ident(std::size_t position) const {
    if (position >= source_.size()) {
        return false;
    }
    const char c = source_[position];
    if (c == '-') {
        if (position + 1 >= source_.size()) {
            return false;
        }
        const char next = source_[position + 1];
        return is_name_start(next) || next == '-' || is_valid_escape(source_, position + 1);
    }
    return is_name_start(c) || is_valid_escape(source_, position);
}

bool Tokenizer::starts_name(std::size_t position) const {
    if (position >= source_.size()) {
        return false;
    }
    return is_name_char(source_[position]) || is_valid_escape(source_, position);
}

bool Tokenizer::starts_number(std::size_t position) const {
    if (position >= source_.size()) {
        return false;
    }
    const char c = source_[position];
    if (is_digit(c)) {
        return true;
    }
    if (c == '.' && position + 1 < source_.size() && is_digit(source_[position + 1])) {
        return true;
    }
    if ((c == '+' || c == '-') && position + 1 < source_.size()) {
        const char next = source_[position + 1];
        if (is_digit(next)) {
            return true;
        }
        return next == '.' && position + 2 < source_.size() && is_digit(source_[position + 2]);
    }
    return false;
}

bool Tokenizer::starts_unicode_range(std::size_t position) const {
    if (position + 2 >= source_.size()) {
        return false;
    }
    const char c = source_[position];
    if ((c != 'u' && c != 'U') || source_[position + 1] != '+') {
        return false;
    }
    const char next = source_[position + 2];
    return is_hex_digit(next) || next == '?';
}

bool Tokenizer::matches(std::size_t position, std::string_view text) const {
    return position + text.size() <= source_.size() &&
           source_.substr(position, text.size()) == text;
}

void Tokenizer::consume_name() {
    while (!at_end()) {
        const char c = source_[position_];
        if (is_name_char(c)) {
            ++position_;
        } else if (is_valid_escape(source_, position_)) {
            consume_escape();
        } else {
            break;
        }
    }
}

void Tokenizer::consume_escape() {
    ++position_;
    if (at_end()) {
        return;
    }
    if (is_hex_digit(source_[position_])) {
        int digits = 0;
        while (!at_end() && digits < 6 && is_hex_digit(source_[position_])) {
            ++position_;
            ++digits;
        }
        if (!at_end() && is_whitespace(source_[position_])) {
            ++position_;
        }
        return;
    }
    ++position_;
}

void Tokenizer::consume_number() {
    if (!at_end() && (source_[position_] == '+' || source_[position_] == '-')) {
        ++position_;
    }
    while (!at_end() && is_digit(source_[position_])) {
        ++position_;
    }
    if (!at_end() && source_[position_] == '.' && position_ + 1 < source_.size() &&
        is_digit(source_[position_ + 1])) {
        ++position_;
        while (!at_end() && is_digit(source_[position_])) {
            ++position_;
        }
    }
    if (!at_end() && (source_[position_] == 'e' || source_[position_] == 'E')) {
        std::size_t exponent = position_ + 1;
        if (exponent < source_.size() && (source_[exponent] == '+' || source_[exponent] == '-')) {
            ++exponent;
        }
        if (exponent < source_.size() && is_digit(source_[exponent])) {
            position_ = exponent + 1;
            while (!at_end() && is_digit(source_[position_])) {
                ++position_;
            }
        }
    }
}

void Tokenizer::consume_unicode_range() {
    position_ += 2;
    int digits = 0;
    while (!at_end() && digits < 6 &&
           (is_hex_digit(source_[position_]) || source_[position_] == '?')) {
        ++position_;
        ++digits;
    }
    if (!at_end() && source_[position_] == '-') {
        ++position_;
        digits = 0;
        while (!at_end() && digits < 6 && is_hex_digit(source_[position_])) {
            ++position_;
            ++digits;
        }
    }
}

} // namespace tessera::ui::style
