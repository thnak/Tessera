// SPDX-License-Identifier: MIT

// String, template and JSX text scanning for Lexer (docs/specs/jsx-subset-v1.md
// sections 3.4, 10.4).

#include "tessera/compiler/jsx/lexer.hpp"
#include "utf8.hpp"

namespace tessera::compiler::jsx {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool is_high_surrogate(char32_t value) {
    return value >= 0xD800 && value <= 0xDBFF;
}

bool is_low_surrogate(char32_t value) {
    return value >= 0xDC00 && value <= 0xDFFF;
}

} // namespace

bool Lexer::read_hex_digits(std::size_t count, char32_t& value) {
    value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (pos_ >= source_.size()) {
            return false;
        }
        const int digit = hex_value(source_[pos_]);
        if (digit < 0) {
            return false;
        }
        value = value * 16 + static_cast<char32_t>(digit);
        ++pos_;
    }
    return true;
}

void Lexer::scan_escape(std::string& out, bool in_template) {
    const std::size_t begin = pos_; // the backslash
    ++pos_;
    if (pos_ >= source_.size()) {
        return; // the caller reports the unterminated literal
    }
    const char c = source_[pos_];
    if (c == '\n' || c == '\r') {
        report("TESSERA_JSX_LEX_INVALID_ESCAPE", "a backslash cannot continue a line", begin,
               begin + 1, "write `\\n` for a line break");
        return;
    }
    ++pos_;
    switch (c) {
    case '\\':
    case '"':
    case '\'':
        out.push_back(c);
        return;
    case 'n':
        out.push_back('\n');
        return;
    case 'r':
        out.push_back('\r');
        return;
    case 't':
        out.push_back('\t');
        return;
    case '`':
    case '$':
        if (in_template) {
            out.push_back(c);
            return;
        }
        break;
    case 'u': {
        char32_t value = 0;
        bool ok = false;
        if (pos_ < source_.size() && source_[pos_] == '{') {
            ++pos_;
            std::size_t digits = 0;
            ok = true;
            while (pos_ < source_.size() && hex_value(source_[pos_]) >= 0) {
                if (digits < 8) {
                    value = value * 16 + static_cast<char32_t>(hex_value(source_[pos_]));
                }
                ++digits;
                ++pos_;
            }
            if (digits == 0 || digits > 6 || pos_ >= source_.size() || source_[pos_] != '}') {
                ok = false;
            } else {
                ++pos_;
            }
            ok = ok && value <= 0x10FFFF;
        } else {
            ok = read_hex_digits(4, value);
            if (ok && is_high_surrogate(value) && pos_ + 1 < source_.size() &&
                source_[pos_] == '\\' && source_[pos_ + 1] == 'u') {
                const std::size_t save = pos_;
                pos_ += 2;
                char32_t low = 0;
                if (read_hex_digits(4, low) && is_low_surrogate(low)) {
                    value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    pos_ = save;
                }
            }
        }
        if (ok && !is_high_surrogate(value) && !is_low_surrogate(value)) {
            append_utf8(out, value);
            return;
        }
        break;
    }
    default:
        break;
    }
    // Keep the escaped code point whole so recovery never splits a UTF-8 sequence.
    std::size_t extra = utf8_sequence_length(source_, pos_ - 1);
    if (extra > 1) {
        pos_ += extra - 1;
    }
    report("TESSERA_JSX_LEX_INVALID_ESCAPE",
           "invalid escape sequence `" + std::string(source_.substr(begin, pos_ - begin)) + "`",
           begin, pos_, "supported escapes: \\\\ \\\" \\' \\n \\r \\t \\uXXXX \\u{X...}");
}

Token Lexer::lex_string(std::size_t begin) {
    const char quote = source_[pos_];
    ++pos_;
    std::string value;
    bool closed = false;
    while (pos_ < source_.size()) {
        const char c = source_[pos_];
        if (c == quote) {
            ++pos_;
            closed = true;
            break;
        }
        if (c == '\n' || c == '\r') {
            break; // a raw line break ends the literal; the break itself is not consumed
        }
        if (c == '\\') {
            scan_escape(value, false);
            continue;
        }
        value.push_back(c);
        ++pos_;
    }
    Token token = make_token(TokenKind::String, begin);
    token.value = std::move(value);
    if (!closed) {
        report("TESSERA_JSX_LEX_UNTERMINATED_STRING", "unterminated string literal", begin,
               begin + 1, std::string("add the closing ") + quote);
    }
    return token;
}

Token Lexer::lex_template_part(std::size_t begin, bool is_start) {
    std::string value;
    TokenKind kind = is_start ? TokenKind::TemplateString : TokenKind::TemplateTail;
    bool closed = false;
    while (pos_ < source_.size()) {
        const char c = source_[pos_];
        if (c == '`') {
            ++pos_;
            closed = true;
            break;
        }
        if (c == '$' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '{') {
            pos_ += 2;
            kind = is_start ? TokenKind::TemplateHead : TokenKind::TemplateMiddle;
            closed = true;
            break;
        }
        if (c == '\\') {
            scan_escape(value, true);
            continue;
        }
        if (c == '\r' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '\n') {
            ++pos_; // CRLF is one LF in cooked text
            continue;
        }
        value.push_back(c);
        ++pos_;
    }
    Token token = make_token(kind, begin);
    token.value = std::move(value);
    if (!closed) {
        report("TESSERA_JSX_LEX_UNTERMINATED_TEMPLATE", "unterminated template literal", begin,
               begin + 1, "add the closing backtick");
    }
    return token;
}

Token Lexer::lex_jsx_text(std::size_t begin) {
    while (pos_ < source_.size() && source_[pos_] != '{' && source_[pos_] != '<') {
        const char c = source_[pos_];
        if (c == '}' || c == '>') {
            report("TESSERA_JSX_LEX_INVALID_TEXT_CHARACTER",
                   std::string("`") + c + "` is not allowed in JSX text", pos_, pos_ + 1,
                   std::string("write {'") + c + "'} to show it as text");
        }
        ++pos_;
    }
    return make_token(TokenKind::JsxText, begin);
}

} // namespace tessera::compiler::jsx
