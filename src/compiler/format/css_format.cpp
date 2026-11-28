// SPDX-License-Identifier: MIT
//
// The CSS printer (ADR-061 slice 3). It prints from the stylesheet engine's own token stream
// (src/ui/style/tokenizer.cpp), so token boundaries are the engine's. It never rewrites a token: it
// only decides whitespace, line breaks and the closing semicolon, and it prints the engine's
// comment tokens where they stand. Comments inside a declaration or a prelude stay inline, so no
// comment is ever dropped or moved to another construct. The result is verified by re-tokenizing.

#include "other_formats.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <utility>
#include <vector>

namespace tessera::compiler::format {
namespace {

using ui::style::Token;
using ui::style::TokenType;

std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

class CssPrinter {
public:
    CssPrinter(std::string_view source, int indent_width)
        : src_(source),
          indent_(indent_width < 1 ? 2 : indent_width),
          toks_(ui::style::Tokenizer(source).tokenize()) {
        if (toks_.empty() || toks_.back().type != TokenType::Eof) {
            Token eof;
            eof.offset = source.size();
            toks_.push_back(eof);
        }
    }

    bool run(std::string& out, Problem& problem) {
        if (!block(0, 0)) {
            problem = std::move(problem_);
            return false;
        }
        if (!out_.empty()) {
            out_.push_back('\n');
        }
        out = std::move(out_);
        return true;
    }

private:
    bool fail(std::size_t offset, std::string message) {
        problem_ = Problem{"TESSERA_FMT_SYNTAX", std::move(message), offset};
        return false;
    }

    std::string pad(int depth) const {
        return std::string(static_cast<std::size_t>(depth * indent_), ' ');
    }

    bool is_ws(std::size_t i) const {
        return toks_[i].type == TokenType::Whitespace;
    }

    std::size_t end_of(std::size_t i) const {
        return toks_[i].offset + toks_[i].text.size();
    }

    std::string_view raw(std::size_t b, std::size_t e) const {
        return src_.substr(toks_[b].offset, end_of(e - 1) - toks_[b].offset);
    }

    void start_line(int depth, bool blank) {
        if (!out_.empty()) {
            out_.push_back('\n');
            if (blank) {
                out_.push_back('\n');
            }
        }
        out_ += pad(depth);
    }

    // A block comment whose continuation lines all start with `*` is re-indented under its first
    // line; any other multi-line comment is printed byte for byte (the same rule as .tsx).
    std::string comment_text(const Token& t, int depth) const {
        const std::string_view text = t.text;
        if (text.find('\n') == std::string_view::npos) {
            return std::string(text);
        }
        std::vector<std::string_view> lines;
        for (std::size_t from = 0;;) {
            const std::size_t nl = text.find('\n', from);
            lines.push_back(text.substr(from, nl == std::string_view::npos ? nl : nl - from));
            if (nl == std::string_view::npos) {
                break;
            }
            from = nl + 1;
        }
        for (std::size_t i = 1; i < lines.size(); ++i) {
            if (!trim_ws(lines[i]).starts_with('*')) {
                return std::string(text);
            }
        }
        std::string out(lines[0]);
        for (std::size_t i = 1; i < lines.size(); ++i) {
            std::string_view line = trim_ws(lines[i]);
            out += "\n" + pad(depth) + " ";
            out += line;
        }
        return out;
    }

    // Items of one block, until its `}` (depth > 0) or the end of the file (depth 0).
    bool block(int depth, std::size_t opener) {
        bool first = true;
        int newlines = 0;
        for (;;) {
            const Token& t = toks_[pos_];
            switch (t.type) {
            case TokenType::Eof:
                return depth == 0 ? true : fail(opener, "unclosed `{`");
            case TokenType::Whitespace:
                for (const char c : t.text) {
                    newlines += c == '\n' ? 1 : 0;
                }
                ++pos_;
                continue;
            case TokenType::RightBrace:
                if (depth == 0) {
                    return fail(t.offset, "unmatched `}`");
                }
                ++pos_;
                return true;
            case TokenType::Semicolon:
                ++pos_;
                continue;
            case TokenType::Comment:
                if (newlines == 0 && !out_.empty()) {
                    out_ += ' ' + comment_text(t, depth);
                } else {
                    start_line(depth, newlines >= 2 && !first);
                    out_ += comment_text(t, depth);
                    first = false;
                }
                break;
            default:
                if (!item(depth, !first && newlines >= 2)) {
                    return false;
                }
                first = false;
                newlines = 0;
                continue;
            }
            ++pos_;
            newlines = 0;
        }
    }

    bool starts_custom(std::size_t b) const {
        return toks_[b].type == TokenType::Ident && toks_[b].text.starts_with("--");
    }

    bool item(int depth, bool blank) {
        const std::size_t b = pos_;
        const bool custom = starts_custom(b);
        int paren = 0;
        std::size_t i = b;
        TokenType term = TokenType::Eof;
        for (;; ++i) {
            const TokenType type = toks_[i].type;
            if (type == TokenType::Eof) {
                if (paren > 0) {
                    return fail(toks_[b].offset, "unclosed `(` or `[`");
                }
                break;
            }
            if (type == TokenType::BadString) {
                return fail(toks_[i].offset, "unterminated string");
            }
            if (type == TokenType::Function || type == TokenType::LeftParen ||
                type == TokenType::LeftBracket) {
                ++paren;
            } else if (type == TokenType::RightParen || type == TokenType::RightBracket) {
                paren = paren > 0 ? paren - 1 : 0;
            } else if (paren == 0 &&
                       (type == TokenType::Semicolon || type == TokenType::RightBrace)) {
                term = type;
                break;
            } else if (paren == 0 && type == TokenType::LeftBrace) {
                if (!custom) {
                    term = type;
                    break;
                }
                int braces = 1; // a custom property value may hold a balanced {} block
                for (++i; braces > 0; ++i) {
                    if (toks_[i].type == TokenType::Eof) {
                        return fail(toks_[b].offset, "unclosed `{`");
                    }
                    braces += toks_[i].type == TokenType::LeftBrace ? 1 : 0;
                    braces -= toks_[i].type == TokenType::RightBrace ? 1 : 0;
                }
                --i;
            }
        }
        std::size_t e = i;
        while (e > b && is_ws(e - 1)) {
            --e;
        }
        start_line(depth, blank);
        if (term == TokenType::LeftBrace) {
            const std::size_t opener = toks_[i].offset;
            out_ += (toks_[b].type == TokenType::AtKeyword ? std::string(raw(b, e))
                                                           : selector(b, e, depth));
            out_ += e == b ? "{" : " {";
            pos_ = i + 1;
            const std::size_t mark = out_.size();
            if (!block(depth + 1, opener)) {
                return false;
            }
            out_ += out_.size() == mark ? "}" : "\n" + pad(depth) + "}";
            return true;
        }
        out_ += statement(b, e, depth);
        out_ += ';';
        pos_ = term == TokenType::Semicolon ? i + 1 : i;
        return true;
    }

    // Whitespace runs collapse to one space. With `keep_breaks` a run that holds a newline outside
    // parentheses stays a line break, continued one level deeper (a multi-line grid or list value).
    std::string render(std::size_t b, std::size_t e, int depth, bool keep_breaks) const {
        std::string out;
        int paren = 0;
        bool space = false;
        bool brk = false;
        for (std::size_t i = b; i < e; ++i) {
            const Token& t = toks_[i];
            if (t.type == TokenType::Whitespace) {
                if (keep_breaks && paren == 0 && t.text.find('\n') != std::string_view::npos) {
                    brk = true;
                } else {
                    space = true;
                }
                continue;
            }
            if (!out.empty()) {
                out += brk ? "\n" + pad(depth + 1) : (space ? " " : "");
            }
            brk = space = false;
            out += t.text;
            if (t.type == TokenType::Function || t.type == TokenType::LeftParen ||
                t.type == TokenType::LeftBracket) {
                ++paren;
            } else if ((t.type == TokenType::RightParen || t.type == TokenType::RightBracket) &&
                       paren > 0) {
                --paren;
            }
        }
        return out;
    }

    // One selector per line; combinators and compound selectors are never respaced.
    std::string selector(std::size_t b, std::size_t e, int depth) const {
        std::string out;
        int paren = 0;
        std::size_t from = b;
        for (std::size_t i = b; i <= e; ++i) {
            if (i < e) {
                const TokenType type = toks_[i].type;
                if (type == TokenType::Function || type == TokenType::LeftParen ||
                    type == TokenType::LeftBracket) {
                    ++paren;
                } else if ((type == TokenType::RightParen || type == TokenType::RightBracket) &&
                           paren > 0) {
                    --paren;
                }
                if (type != TokenType::Comma || paren != 0) {
                    continue;
                }
            }
            std::size_t pb = from;
            std::size_t pe = i;
            while (pb < pe && is_ws(pb)) {
                ++pb;
            }
            while (pe > pb && is_ws(pe - 1)) {
                --pe;
            }
            if (from != b) {
                out += ",\n" + pad(depth);
            }
            out += render(pb, pe, depth, false);
            from = i + 1;
        }
        return out;
    }

    // `prop: value;` for a declaration, the tokens with collapsed whitespace for anything else. An
    // @-rule prelude and a custom property value are printed untouched.
    std::string statement(std::size_t b, std::size_t e, int depth) const {
        if (toks_[b].type == TokenType::AtKeyword) {
            return std::string(raw(b, e));
        }
        std::size_t k = b + 1;
        while (k < e && is_ws(k)) {
            ++k;
        }
        if (toks_[b].type != TokenType::Ident || k >= e || toks_[k].type != TokenType::Colon) {
            return render(b, e, depth, false);
        }
        std::size_t vb = k + 1;
        while (vb < e && is_ws(vb)) {
            ++vb;
        }
        std::string value =
            vb >= e ? std::string()
                    : (starts_custom(b) ? std::string(raw(vb, e)) : render(vb, e, depth, true));
        return std::string(toks_[b].text) + ":" + (value.empty() ? "" : " " + value);
    }

    std::string_view src_;
    int indent_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    std::string out_;
    Problem problem_;
};

} // namespace

bool format_css(std::string_view source, const FormatOptions& options, std::string& out,
                Problem& problem) {
    std::string formatted;
    CssPrinter printer(source, options.indent_width);
    if (!printer.run(formatted, problem) || !css_same_meaning(source, formatted, problem)) {
        return false;
    }
    out = std::move(formatted);
    return true;
}

} // namespace tessera::compiler::format
