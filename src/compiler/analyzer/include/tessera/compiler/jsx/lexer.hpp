// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/limits.hpp"
#include "tessera/compiler/jsx/token.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::jsx {

// Tolerant, parser-driven lexer for the JSX/TSX subset (docs/specs/jsx-subset-v1.md section 3).
//
// Every malformed construct reports a diagnostic and still yields a token, so one call to
// next() always consumes at least one byte or returns Eof; there is no input that loops.
// Source-wide checks (invalid UTF-8, lone CR, file size) run once in the constructor. After
// Limits::max_tokens tokens the lexer reports TESSERA_JSX_LIMIT_TOKENS and returns Eof forever.
class Lexer {
public:
    // Snapshot for speculative parsing; the caller rewinds the DiagnosticSink separately.
    struct Mark {
        std::size_t position = 0;
        std::size_t comments = 0;
        std::size_t tokens = 0;
        bool stopped = false;
    };

    Lexer(std::string_view source, DiagnosticSink& sink, const Limits& limits);

    Token next(LexMode mode);

    // Scans the rest of a template literal after the `}` that closes a substitution. The caller
    // passes the `}` token; the result is TemplateMiddle or TemplateTail.
    Token continue_template(const Token& close_brace);

    [[nodiscard]] Mark mark() const {
        return Mark{pos_, comments_.size(), tokens_, stopped_};
    }
    void rewind(const Mark& mark);

    [[nodiscard]] const std::vector<Comment>& comments() const {
        return comments_;
    }
    // Moves the read position forward inside the token just returned. Used to split `>>` into two
    // `>` when closing nested generic arguments.
    void seek(std::size_t position) {
        pos_ = position;
    }

    // True once the lexer gave up (file size or token limit); it has already reported why.
    [[nodiscard]] bool stopped() const {
        return stopped_;
    }
    [[nodiscard]] std::vector<Comment> take_comments() {
        return std::move(comments_);
    }

private:
    Token make_token(TokenKind kind, std::size_t begin) const;
    Token eof_token() const;
    void skip_trivia();
    Token lex_identifier(LexMode mode, std::size_t begin);
    Token lex_number(std::size_t begin);
    Token lex_string(std::size_t begin);
    Token lex_template_part(std::size_t begin, bool is_start);
    Token lex_punctuator(LexMode mode, std::size_t begin);
    Token lex_jsx_text(std::size_t begin);
    // Decodes the escape at pos_ (which is a backslash) into `out`. Reports and recovers on error.
    void scan_escape(std::string& out, bool in_template);
    bool read_hex_digits(std::size_t count, char32_t& value);
    void report(std::string_view code, std::string message, std::size_t begin, std::size_t end,
                std::string suggestion = {});

    std::string_view source_;
    DiagnosticSink* sink_;
    Limits limits_;
    std::size_t pos_ = 0;
    std::size_t tokens_ = 0;
    bool stopped_ = false;
    std::vector<Comment> comments_;
};

} // namespace tessera::compiler::jsx
