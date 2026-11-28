// SPDX-License-Identifier: MIT

#include "scan.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/limits.hpp"
#include "tessera/compiler/jsx/source_map.hpp"

#include <utility>

namespace tessera::compiler::format {
namespace {

using jsx::LexMode;
using jsx::Token;
using jsx::TokenKind;

enum class FrameKind : std::uint8_t { Code, Tag, Child, Subst };

struct Frame {
    FrameKind kind = FrameKind::Code;
    int braces = 0;               // Code: nested `{` opened inside this frame
    bool top = false;             // Code: the file level, never popped
    bool closing = false;         // Tag: a `</...>` tag
    bool seen_first = false;      // Tag: a token after `<` has been read
    std::uint32_t atom_begin = 0; // Subst
    std::size_t first_token = 0;  // Subst: tokens index where the template began
};

// Whether a token can end an operand, which decides between `<` as a JSX start and as a
// comparison or generic bracket (the same question a lexer asks about `/` and regular expressions).
bool ends_operand(const FTok& tok) {
    switch (tok.kind) {
    case FKind::Number:
    case FKind::String:
    case FKind::Atom:
        return true;
    case FKind::Word:
        return !tok.keyword || tok.text == "true" || tok.text == "false" || tok.text == "null" ||
               tok.text == "this";
    case FKind::Punct:
        return tok.text == ")" || tok.text == "]" || tok.text == ">" || tok.text == "/>";
    default:
        return false;
    }
}

class Scanner {
public:
    explicit Scanner(std::string_view source)
        : source_(source),
          map_(source),
          sink_(map_, limits_),
          lexer_(source, sink_, limits_) {}

    ScanResult run() {
        stack_.push_back(Frame{FrameKind::Code, 0, true, false, false, 0, 0});
        while (result_.error.empty()) {
            if (!step()) {
                break;
            }
        }
        if (result_.error.empty() && (stack_.size() != 1 || lexer_.stopped())) {
            fail("unbalanced JSX or template while scanning", 0);
        }
        result_.comments.reserve(lexer_.comments().size());
        for (const jsx::Comment& comment : lexer_.comments()) {
            result_.comments.push_back(
                FComment{comment.span.offset, comment.span.end(), comment.block,
                         source_.substr(comment.span.offset, comment.span.length), false});
        }
        return std::move(result_);
    }

private:
    void fail(std::string message, std::uint32_t offset) {
        if (result_.error.empty()) {
            result_.error = std::move(message);
            result_.error_offset = offset;
        }
    }

    FTok& emit(const Token& token, FKind kind, LexMode mode) {
        FTok tok;
        tok.kind = kind;
        tok.mode = mode;
        tok.keyword = token.kind == TokenKind::Keyword;
        tok.begin = token.span.offset;
        tok.end = token.span.end();
        tok.text = source_.substr(token.span.offset, token.span.length);
        result_.tokens.push_back(std::move(tok));
        return result_.tokens.back();
    }

    void emit_atom(std::uint32_t begin, std::uint32_t end) {
        FTok tok;
        tok.kind = FKind::Atom;
        tok.begin = begin;
        tok.end = end;
        tok.text = source_.substr(begin, end - begin);
        result_.tokens.push_back(std::move(tok));
    }

    static FKind kind_of(const Token& token) {
        switch (token.kind) {
        case TokenKind::Identifier:
        case TokenKind::Keyword:
            return FKind::Word;
        case TokenKind::Number:
            return FKind::Number;
        case TokenKind::String:
            return FKind::String;
        case TokenKind::JsxText:
            return FKind::JsxText;
        case TokenKind::Eof:
            return FKind::Eof;
        default:
            return FKind::Punct;
        }
    }

    bool step() {
        const FrameKind kind = stack_.back().kind;
        if (kind == FrameKind::Child) {
            return step_child();
        }
        if (kind == FrameKind::Tag) {
            return step_tag();
        }
        return step_code();
    }

    bool step_code() {
        const Token token = lexer_.next(LexMode::Code);
        if (token.kind == TokenKind::Eof) {
            FTok eof;
            eof.begin = static_cast<std::uint32_t>(source_.size());
            eof.end = eof.begin;
            result_.tokens.push_back(std::move(eof));
            return false;
        }
        if (token.kind == TokenKind::Unknown) {
            fail("unexpected character", token.span.offset);
            return false;
        }
        if (token.kind == TokenKind::TemplateString) {
            emit_atom(token.span.offset, token.span.end());
            return true;
        }
        if (token.kind == TokenKind::TemplateHead) {
            stack_.push_back(Frame{FrameKind::Subst, 0, false, false, false, token.span.offset,
                                   result_.tokens.size()});
            stack_.push_back(Frame{FrameKind::Code, 0, false, false, false, 0, 0});
            return true;
        }
        Frame& frame = stack_.back();
        if (token.kind == TokenKind::Punct && token.text == "{") {
            ++frame.braces;
        } else if (token.kind == TokenKind::Punct && token.text == "}") {
            if (frame.braces > 0) {
                --frame.braces;
            } else if (!frame.top) {
                return close_container(token);
            }
        } else if (token.kind == TokenKind::Punct && token.text == "<" &&
                   (result_.tokens.empty() || !ends_operand(result_.tokens.back()))) {
            FTok& tok = emit(token, FKind::Punct, LexMode::Code);
            tok.jsx_lt = true;
            stack_.push_back(Frame{FrameKind::Tag, 0, false, false, false, 0, 0});
            return true;
        }
        emit(token, kind_of(token), LexMode::Code);
        return true;
    }

    // A `}` that closes a template substitution or a JSX expression container.
    bool close_container(const Token& close) {
        stack_.pop_back();
        if (stack_.back().kind == FrameKind::Subst) {
            const Token part = lexer_.continue_template(close);
            if (part.kind == TokenKind::TemplateMiddle) {
                stack_.push_back(Frame{FrameKind::Code, 0, false, false, false, 0, 0});
                return true;
            }
            if (part.kind != TokenKind::TemplateTail) {
                fail("unterminated template literal", close.span.offset);
                return false;
            }
            const Frame subst = stack_.back();
            stack_.pop_back();
            result_.tokens.resize(subst.first_token);
            emit_atom(subst.atom_begin, part.span.end());
            return true;
        }
        emit(close, FKind::Punct, LexMode::Code);
        return true;
    }

    bool step_tag() {
        const Token token = lexer_.next(LexMode::JsxTag);
        if (token.kind == TokenKind::Eof) {
            fail("unterminated JSX tag", static_cast<std::uint32_t>(source_.size()));
            return false;
        }
        if (token.kind == TokenKind::Unknown) {
            fail("unexpected character", token.span.offset);
            return false;
        }
        Frame& frame = stack_.back();
        if (token.kind == TokenKind::Punct && token.text == "/" && !frame.seen_first) {
            frame.closing = true;
        }
        // `</>` lexes as `<` and `/>`: the closing tag of a fragment.
        if (token.kind == TokenKind::Punct && token.text == "/>" && !frame.seen_first) {
            frame.closing = true;
        }
        frame.seen_first = true;
        emit(token, kind_of(token), LexMode::JsxTag);
        if (token.kind != TokenKind::Punct) {
            return true;
        }
        if (token.text == "{") {
            stack_.push_back(Frame{FrameKind::Code, 0, false, false, false, 0, 0});
        } else if (token.text == "/>" || token.text == ">") {
            const bool closing = frame.closing;
            const bool self_closing = token.text == "/>" && !closing;
            stack_.pop_back();
            if (self_closing) {
                return true;
            }
            if (closing) {
                if (stack_.empty() || stack_.back().kind != FrameKind::Child) {
                    fail("closing tag without an element", token.span.offset);
                    return false;
                }
                stack_.pop_back();
            } else {
                stack_.push_back(Frame{FrameKind::Child, 0, false, false, false, 0, 0});
            }
        }
        return true;
    }

    bool step_child() {
        const Token token = lexer_.next(LexMode::JsxChild);
        if (token.kind == TokenKind::Eof) {
            fail("unterminated JSX element", static_cast<std::uint32_t>(source_.size()));
            return false;
        }
        FTok& tok = emit(token, kind_of(token), LexMode::JsxChild);
        if (token.kind == TokenKind::Punct && token.text == "{") {
            stack_.push_back(Frame{FrameKind::Code, 0, false, false, false, 0, 0});
        } else if (token.kind == TokenKind::Punct && token.text == "<") {
            tok.jsx_lt = true;
            stack_.push_back(Frame{FrameKind::Tag, 0, false, false, false, 0, 0});
        }
        return true;
    }

    std::string_view source_;
    jsx::Limits limits_;
    jsx::SourceMap map_;
    jsx::DiagnosticSink sink_;
    jsx::Lexer lexer_;
    std::vector<Frame> stack_;
    ScanResult result_;
};

bool has_newline(std::string_view source, std::uint32_t begin, std::uint32_t end) {
    for (std::uint32_t i = begin; i < end; ++i) {
        if (source[i] == '\n') {
            return true;
        }
    }
    return false;
}

bool has_blank_line(std::string_view source, std::uint32_t begin, std::uint32_t end) {
    int newlines = 0;
    for (std::uint32_t i = begin; i < end; ++i) {
        if (source[i] == '\n' && ++newlines >= 2) {
            return true;
        }
    }
    return false;
}

void match_brackets(ScanResult& result) {
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < result.tokens.size() && result.error.empty(); ++i) {
        FTok& tok = result.tokens[i];
        if (tok.kind != FKind::Punct) {
            continue;
        }
        if (tok.text == "(" || tok.text == "[" || tok.text == "{") {
            open.push_back(i);
        } else if (tok.text == ")" || tok.text == "]" || tok.text == "}") {
            if (open.empty()) {
                result.error = "unbalanced bracket";
                result.error_offset = tok.begin;
                return;
            }
            const std::size_t opener = open.back();
            open.pop_back();
            const char want = tok.text == ")" ? '(' : tok.text == "]" ? '[' : '{';
            if (result.tokens[opener].text[0] != want) {
                result.error = "mismatched bracket";
                result.error_offset = tok.begin;
                return;
            }
            result.tokens[opener].match = static_cast<std::int32_t>(i);
            tok.match = static_cast<std::int32_t>(opener);
        }
    }
    if (result.error.empty() && !open.empty()) {
        result.error = "unbalanced bracket";
        result.error_offset = result.tokens[open.back()].begin;
    }
}

// `Foo<int>(x)` and `a < b` use the same characters. A `<` glued to the identifier before it whose
// matching `>` is reached through type-like tokens only is a type-argument list.
void mark_generics(ScanResult& result) {
    auto& toks = result.tokens;
    for (std::size_t i = 1; i < toks.size(); ++i) {
        const FTok& open = toks[i];
        const FTok& prev = toks[i - 1];
        if (!open.is("<") || open.jsx_lt || open.mode != LexMode::Code ||
            prev.kind != FKind::Word || prev.keyword || prev.end != open.begin) {
            continue;
        }
        int depth = 1;
        std::size_t close = 0;
        for (std::size_t j = i + 1; j < toks.size() && depth > 0; ++j) {
            const FTok& t = toks[j];
            if (t.kind == FKind::Word || t.kind == FKind::String || t.kind == FKind::Number) {
                continue;
            }
            if (t.kind != FKind::Punct) {
                break;
            }
            if ((t.text == "(" || t.text == "[" || t.text == "{") && t.match > 0) {
                j = static_cast<std::size_t>(t.match);
            } else if (t.text == "<") {
                ++depth;
            } else if (t.text == ">" || t.text == ">>" || t.text == ">>>") {
                depth -= static_cast<int>(t.text.size());
                if (depth <= 0) {
                    close = j;
                }
            } else if (t.text != "," && t.text != "." && t.text != "|" && t.text != "&" &&
                       t.text != "[" && t.text != "]") {
                break;
            }
        }
        if (depth == 0 && close != 0) {
            toks[i].generic = true;
            toks[close].generic = true;
        }
    }
}

// Own-line comments lead the next token, same-line comments trail the previous one (ADR-061).
void attach_comments(std::string_view source, ScanResult& result) {
    std::size_t next_token = 0;
    std::uint32_t previous_end = 0; // end of the last token or comment seen
    bool have_previous_token = false;
    std::uint32_t previous_token_end = 0;
    for (std::uint32_t id = 0; id < result.comments.size(); ++id) {
        FComment& comment = result.comments[id];
        while (next_token < result.tokens.size() && result.tokens[next_token].kind != FKind::Eof &&
               result.tokens[next_token].end <= comment.begin) {
            have_previous_token = true;
            previous_token_end = result.tokens[next_token].end;
            previous_end = std::max(previous_end, previous_token_end);
            ++next_token;
        }
        FTok& following = result.tokens[next_token];
        if (following.kind != FKind::Eof && following.begin < comment.end) {
            // The comment sits inside a token printed verbatim (a template literal).
            result.error = "comment inside a template literal";
            result.error_offset = comment.begin;
            result.error_is_comment = true;
            return;
        }
        const bool same_line =
            have_previous_token && !has_newline(source, previous_token_end, comment.begin);
        if (same_line) {
            // Trailing: it follows the previous token on its own line, whatever comes next.
            result.tokens[next_token - 1].trailing.push_back(id);
        } else {
            following.leading.push_back(
                LeadComment{id, has_blank_line(source, previous_end, comment.begin)});
        }
        previous_end = comment.end;
    }
    // The gap before each token is measured from the last comment that precedes it.
    std::uint32_t last_end = 0;
    std::size_t comment_index = 0;
    for (FTok& tok : result.tokens) {
        while (comment_index < result.comments.size() &&
               result.comments[comment_index].end <= tok.begin) {
            last_end = std::max(last_end, result.comments[comment_index].end);
            ++comment_index;
        }
        tok.blank_before = has_blank_line(source, last_end, tok.begin);
        last_end = std::max(last_end, tok.end);
    }
}

} // namespace

ScanResult scan(std::string_view source) {
    ScanResult result = Scanner(source).run();
    if (!result.error.empty()) {
        return result;
    }
    match_brackets(result);
    if (result.error.empty()) {
        mark_generics(result);
        attach_comments(source, result);
    }
    return result;
}

} // namespace tessera::compiler::format
