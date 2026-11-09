// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/lexer.hpp"
#include "utf8.hpp"

#include <charconv>
#include <limits>
#include <system_error>

namespace tessera::compiler::jsx {
namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

bool is_ident_part(char c) {
    return is_ident_start(c) || is_digit(c);
}

// Section 3.3: the words the subset uses plus the JavaScript/TypeScript words it rejects, so that
// the parser can name the rejected construct instead of reporting a generic syntax error.
constexpr std::string_view kKeywords[] = {
    "import", "export",   "from",       "function", "const",   "return", "type",      "true",
    "false",  "null",     "as",         "class",    "extends", "let",    "var",       "if",
    "else",   "for",      "while",      "do",       "switch",  "case",   "default",   "try",
    "catch",  "finally",  "throw",      "await",    "async",   "yield",  "new",       "this",
    "delete", "typeof",   "instanceof", "in",       "void",    "enum",   "interface", "super",
    "with",   "debugger", "break",      "continue",
};

bool is_keyword(std::string_view word) {
    for (const auto keyword : kKeywords) {
        if (keyword == word) {
            return true;
        }
    }
    return false;
}

// Longest first so maximal munch works by trying in order. Includes operators the subset rejects:
// the parser reports them with their specific codes.
constexpr std::string_view kPunctuators[] = {
    ">>>=", "===", "!==", "...", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=", "=>",
    "==",   "!=",  "<=",  ">=",  "&&",  "||",  "++",  "--",  "+=",  "-=",  "*=",   "/=",
    "%=",   "??",  "?.",  "**",  "<<",  ">>",  "&=",  "|=",  "^=",  "(",   ")",    "{",
    "}",    "[",   "]",   ",",   ";",   ":",   ".",   "=",   "?",   "+",   "-",    "*",
    "/",    "%",   "<",   ">",   "!",   "&",   "|",   "^",   "~",
};

} // namespace

Lexer::Lexer(std::string_view source, DiagnosticSink& sink, const Limits& limits)
    : source_(source),
      sink_(&sink),
      limits_(limits) {
    if (source_.size() > limits_.max_file_bytes ||
        source_.size() > std::numeric_limits<std::uint32_t>::max()) {
        report("TESSERA_JSX_LIMIT_FILE_SIZE",
               "file is " + std::to_string(source_.size()) + " bytes; the limit is " +
                   std::to_string(limits_.max_file_bytes),
               0, 0, "split the file");
        stopped_ = true;
        return;
    }
    if (source_.size() >= 3 && static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        pos_ = 3;
    }
    for (std::size_t i = 0; i < source_.size();) {
        const char c = source_[i];
        if (static_cast<unsigned char>(c) >= 0x80U) {
            const std::size_t length = utf8_sequence_length(source_, i);
            if (length == 0) {
                report("TESSERA_JSX_LEX_INVALID_UTF8", "invalid UTF-8 byte sequence", i, i + 1);
                ++i;
            } else {
                i += length;
            }
            continue;
        }
        if (c == '\r' && (i + 1 >= source_.size() || source_[i + 1] != '\n')) {
            report("TESSERA_JSX_LEX_LONE_CR", "a lone CR is not a line break", i, i + 1,
                   "use LF or CRLF line endings");
        }
        ++i;
    }
}

void Lexer::rewind(const Mark& mark) {
    pos_ = mark.position;
    comments_.resize(mark.comments);
    tokens_ = mark.tokens;
    stopped_ = mark.stopped;
}

void Lexer::report(std::string_view code, std::string message, std::size_t begin, std::size_t end,
                   std::string suggestion) {
    sink_->error(code, std::move(message),
                 Span{static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin)},
                 std::move(suggestion));
}

Token Lexer::make_token(TokenKind kind, std::size_t begin) const {
    Token token;
    token.kind = kind;
    token.span = Span{static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(pos_ - begin)};
    token.text = source_.substr(begin, pos_ - begin);
    return token;
}

Token Lexer::eof_token() const {
    Token token;
    token.span = Span{static_cast<std::uint32_t>(source_.size()), 0};
    return token;
}

Token Lexer::next(LexMode mode) {
    if (stopped_) {
        return eof_token();
    }
    Token token;
    if (mode == LexMode::JsxChild) {
        const std::size_t begin = pos_;
        if (pos_ >= source_.size()) {
            return eof_token();
        }
        if (source_[pos_] == '{' || source_[pos_] == '<') {
            ++pos_;
            token = make_token(TokenKind::Punct, begin);
        } else {
            token = lex_jsx_text(begin);
        }
    } else {
        skip_trivia();
        if (pos_ >= source_.size()) {
            return eof_token();
        }
        const std::size_t begin = pos_;
        const char c = source_[pos_];
        if (is_ident_start(c)) {
            token = lex_identifier(mode, begin);
        } else if (is_digit(c)) {
            token = lex_number(begin);
        } else if (c == '"' || c == '\'') {
            token = lex_string(begin);
        } else if (c == '`' && mode == LexMode::Code) {
            ++pos_;
            token = lex_template_part(begin, true);
        } else {
            token = lex_punctuator(mode, begin);
        }
    }
    if (++tokens_ > limits_.max_tokens) {
        report("TESSERA_JSX_LIMIT_TOKENS",
               "file has more than " + std::to_string(limits_.max_tokens) + " tokens",
               token.span.offset, token.span.end(), "split the file");
        stopped_ = true;
        return eof_token();
    }
    return token;
}

Token Lexer::continue_template(const Token& close_brace) {
    if (stopped_) {
        return eof_token();
    }
    pos_ = close_brace.span.end();
    Token token = lex_template_part(close_brace.span.offset, false);
    if (++tokens_ > limits_.max_tokens) {
        report("TESSERA_JSX_LIMIT_TOKENS",
               "file has more than " + std::to_string(limits_.max_tokens) + " tokens",
               token.span.offset, token.span.end(), "split the file");
        stopped_ = true;
        return eof_token();
    }
    return token;
}

void Lexer::skip_trivia() {
    while (pos_ < source_.size()) {
        const char c = source_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++pos_;
        } else if (c == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
            const std::size_t begin = pos_;
            std::size_t end = pos_ + 2;
            while (end < source_.size() && source_[end] != '\n') {
                ++end;
            }
            std::size_t text_end = end;
            if (text_end > begin + 2 && source_[text_end - 1] == '\r') {
                --text_end; // the CR of a CRLF belongs to the line break
            }
            comments_.push_back(Comment{Span{static_cast<std::uint32_t>(begin),
                                             static_cast<std::uint32_t>(text_end - begin)},
                                        false, source_.substr(begin + 2, text_end - begin - 2)});
            pos_ = end;
        } else if (c == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            const std::size_t begin = pos_;
            const std::size_t close = source_.find("*/", pos_ + 2);
            if (close == std::string_view::npos) {
                report("TESSERA_JSX_LEX_UNTERMINATED_COMMENT", "unterminated block comment", begin,
                       begin + 2, "close the comment with `*/`");
                comments_.push_back(
                    Comment{Span{static_cast<std::uint32_t>(begin),
                                 static_cast<std::uint32_t>(source_.size() - begin)},
                            true, source_.substr(begin + 2)});
                pos_ = source_.size();
            } else {
                comments_.push_back(Comment{Span{static_cast<std::uint32_t>(begin),
                                                 static_cast<std::uint32_t>(close + 2 - begin)},
                                            true, source_.substr(begin + 2, close - begin - 2)});
                pos_ = close + 2;
            }
        } else {
            return;
        }
    }
}

Token Lexer::lex_identifier(LexMode mode, std::size_t begin) {
    ++pos_;
    while (pos_ < source_.size() &&
           (is_ident_part(source_[pos_]) || (mode == LexMode::JsxTag && source_[pos_] == '-'))) {
        ++pos_;
    }
    Token token = make_token(TokenKind::Identifier, begin);
    if (mode == LexMode::Code && is_keyword(token.text)) {
        token.kind = TokenKind::Keyword;
    }
    return token;
}

Token Lexer::lex_number(std::size_t begin) {
    while (pos_ < source_.size() && is_digit(source_[pos_])) {
        ++pos_;
    }
    if (pos_ + 1 < source_.size() && source_[pos_] == '.' && is_digit(source_[pos_ + 1])) {
        ++pos_;
        while (pos_ < source_.size() && is_digit(source_[pos_])) {
            ++pos_;
        }
    }
    if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
        std::size_t probe = pos_ + 1;
        if (probe < source_.size() && (source_[probe] == '+' || source_[probe] == '-')) {
            ++probe;
        }
        if (probe < source_.size() && is_digit(source_[probe])) {
            while (probe < source_.size() && is_digit(source_[probe])) {
                ++probe;
            }
            pos_ = probe;
        }
    }
    bool invalid = false;
    if (pos_ < source_.size() && is_ident_part(source_[pos_])) {
        // 0x1F, 1_000, 10n, 1e: swallow the whole run so one literal gives one diagnostic.
        while (pos_ < source_.size() && is_ident_part(source_[pos_])) {
            ++pos_;
        }
        invalid = true;
    }
    Token token = make_token(TokenKind::Number, begin);
    if (invalid) {
        report("TESSERA_JSX_LEX_INVALID_NUMBER",
               "unsupported number literal `" + std::string(token.text) + "`", begin, pos_,
               "write a decimal number such as 42, 0.5 or 1e3");
        return token;
    }
    const auto result =
        std::from_chars(token.text.data(), token.text.data() + token.text.size(), token.number);
    if (result.ec == std::errc::result_out_of_range) {
        token.number = 0.0;
        report("TESSERA_JSX_LEX_INVALID_NUMBER",
               "number literal `" + std::string(token.text) + "` is out of range", begin, pos_);
    }
    return token;
}

Token Lexer::lex_punctuator(LexMode mode, std::size_t begin) {
    const std::string_view rest = source_.substr(begin);
    if (mode == LexMode::JsxTag) {
        if (rest.starts_with("/>")) {
            pos_ += 2;
            return make_token(TokenKind::Punct, begin);
        }
        // Single `<`, `>` and `/` so `<A b={x}/>` and `<>` never merge into operators.
        if (rest[0] == '<' || rest[0] == '>' || rest[0] == '/') {
            ++pos_;
            return make_token(TokenKind::Punct, begin);
        }
    }
    for (const auto punctuator : kPunctuators) {
        if (!rest.starts_with(punctuator)) {
            continue;
        }
        if (punctuator == "?." && rest.size() > 2 && is_digit(rest[2])) {
            continue; // `a?.5:1` is a conditional, as in JavaScript
        }
        pos_ += punctuator.size();
        return make_token(TokenKind::Punct, begin);
    }
    std::size_t length = utf8_sequence_length(source_, begin);
    if (length == 0) {
        length = 1;
    }
    pos_ += length;
    Token token = make_token(TokenKind::Unknown, begin);
    // Invalid UTF-8 was already reported by the constructor.
    if (length > 1 || static_cast<unsigned char>(rest[0]) < 0x80U) {
        report("TESSERA_JSX_LEX_INVALID_CHARACTER",
               "unexpected character `" + std::string(token.text) + "`", begin, pos_);
    }
    return token;
}

} // namespace tessera::compiler::jsx
