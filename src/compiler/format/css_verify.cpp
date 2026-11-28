// SPDX-License-Identifier: MIT
//
// The CSS printer's self-check (ADR-061 slice 3): the printed text is re-tokenized and must carry
// the same comments and the same significant tokens as the input, or the formatter refuses to
// answer.

#include "other_formats.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <utility>
#include <vector>

namespace tessera::compiler::format {
namespace {

using ui::style::Token;
using ui::style::TokenType;

std::string strip_space(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n') {
            out.push_back(c);
        }
    }
    return out;
}

// Comments and significant tokens, as strings. A semicolon that is empty (after `{`, `;` or `}`) or
// that only closes the last declaration (before `}` or the end) is not a token the author wrote,
// and the printer adds and removes exactly those.
struct Signature {
    std::vector<std::string> comments;
    std::vector<std::string> tokens;
};

Signature signature(std::string_view source) {
    Signature sig;
    const std::vector<Token> toks = ui::style::Tokenizer(source).tokenize();
    std::vector<const Token*> sig_toks;
    for (const Token& t : toks) {
        if (t.type == TokenType::Comment) {
            sig.comments.push_back(strip_space(t.text));
        } else if (t.type != TokenType::Whitespace && t.type != TokenType::Eof) {
            sig_toks.push_back(&t);
        }
    }
    for (std::size_t i = 0; i < sig_toks.size(); ++i) {
        const Token& t = *sig_toks[i];
        if (t.type == TokenType::Semicolon) {
            const bool after_open = i == 0 || sig_toks[i - 1]->type == TokenType::Semicolon ||
                                    sig_toks[i - 1]->type == TokenType::LeftBrace ||
                                    sig_toks[i - 1]->type == TokenType::RightBrace;
            const bool before_close =
                i + 1 == sig_toks.size() || sig_toks[i + 1]->type == TokenType::RightBrace;
            if (after_open || before_close) {
                continue;
            }
        }
        sig.tokens.push_back(std::to_string(static_cast<int>(t.type)) + ":" + std::string(t.text));
    }
    return sig;
}

} // namespace

bool css_same_meaning(std::string_view before_text, std::string_view after_text, Problem& problem) {
    const Signature before = signature(before_text);
    const Signature after = signature(after_text);
    if (before.comments != after.comments) {
        problem = Problem{"TESSERA_FMT_VERIFY",
                          "the printed output changed the comments; this is a formatter bug", 0};
        return false;
    }
    if (before.tokens != after.tokens) {
        problem = Problem{"TESSERA_FMT_VERIFY",
                          "the printed output does not mean the same as the input; this is a "
                          "formatter bug",
                          0};
        return false;
    }
    return true;
}

} // namespace tessera::compiler::format
