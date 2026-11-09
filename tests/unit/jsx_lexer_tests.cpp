// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/lexer.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace tessera::compiler::jsx;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(const std::string& actual, const std::string& expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

const char* kind_name(TokenKind kind) {
    switch (kind) {
    case TokenKind::Eof:
        return "eof";
    case TokenKind::Identifier:
        return "id";
    case TokenKind::Keyword:
        return "kw";
    case TokenKind::Number:
        return "num";
    case TokenKind::String:
        return "str";
    case TokenKind::TemplateString:
        return "tpl";
    case TokenKind::TemplateHead:
        return "tpl-head";
    case TokenKind::TemplateMiddle:
        return "tpl-mid";
    case TokenKind::TemplateTail:
        return "tpl-tail";
    case TokenKind::Punct:
        return "p";
    case TokenKind::JsxText:
        return "text";
    case TokenKind::Unknown:
        return "?";
    }
    return "?";
}

struct Lexed {
    std::vector<Token> tokens;
    std::vector<Comment> comments;
    std::vector<Diagnostic> diagnostics;

    std::string codes() const {
        std::string out;
        for (const auto& diagnostic : diagnostics) {
            out += std::string(diagnostic.code) + ";";
        }
        return out;
    }
    std::string dump() const {
        std::string out;
        for (const auto& token : tokens) {
            out += std::string(kind_name(token.kind)) + ":" + std::string(token.text) + " ";
        }
        return out;
    }
};

// Lexes until Eof in one mode. Bounded by size + 2 so a lexer that stops making progress fails
// the test instead of hanging it.
Lexed lex(std::string_view source, LexMode mode = LexMode::Code, const Limits& limits = {}) {
    SourceMap map(source);
    DiagnosticSink sink(map, limits);
    Lexer lexer(source, sink, limits);
    Lexed result;
    for (std::size_t i = 0; i < source.size() + 2; ++i) {
        Token token = lexer.next(mode);
        if (token.kind == TokenKind::Eof) {
            break;
        }
        result.tokens.push_back(std::move(token));
    }
    result.comments = lexer.take_comments();
    result.diagnostics = sink.take();
    return result;
}

void test_identifiers_keywords_and_punctuators() {
    const auto lexed = lex("const x1 = $a_b + foo(1, \"s\");");
    check_eq(lexed.dump(), "kw:const id:x1 p:= id:$a_b p:+ id:foo p:( num:1 p:, str:\"s\" p:) p:; ",
             "token stream");
    check(lexed.diagnostics.empty(), "no diagnostics for clean input");
}

void test_rejected_operators_are_lexed_for_the_parser() {
    const auto lexed = lex("a == b != c ?? d ?. e ** f ++ g += h => i === j !== k ...l");
    check_eq(lexed.dump(),
             "id:a p:== id:b p:!= id:c p:?? id:d p:?. id:e p:** id:f p:++ id:g p:+= id:h p:=> "
             "id:i p:=== id:j p:!== id:k p:... id:l ",
             "maximal munch operators");
    check(lexed.diagnostics.empty(), "operators are the parser's to reject, not the lexer's");

    check_eq(lex("a & b | c ^ ~d").dump(), "id:a p:& id:b p:| id:c p:^ p:~ id:d ",
             "single-character bitwise operators");

    const auto conditional = lex("a?.5:1");
    check_eq(conditional.dump(), "id:a p:? p:. num:5 p:: num:1 ", "?.5 stays a conditional");
}

void test_numbers() {
    const auto lexed = lex("0 42 0.5 1e3 1E+2 2.5e-1");
    check(lexed.diagnostics.empty(), "valid numbers");
    check(lexed.tokens.size() == 6, "six numbers");
    check(lexed.tokens[2].number == 0.5 && lexed.tokens[3].number == 1000.0 &&
              lexed.tokens[4].number == 100.0 && lexed.tokens[5].number == 0.25,
          "number values");

    for (const char* bad : {"0x1F", "1_000", "10n", "1e", "0b1"}) {
        const auto result = lex(bad);
        check_eq(result.codes(), "TESSERA_JSX_LEX_INVALID_NUMBER;",
                 std::string("one diagnostic for ") + bad);
        check(result.tokens.size() == 1 && result.tokens[0].text == bad,
              std::string("whole run is one token for ") + bad);
    }
    check_eq(lex("1e999").codes(), "TESSERA_JSX_LEX_INVALID_NUMBER;", "out of range number");
    check_eq(lex("1.").dump(), "num:1 p:. ", "a trailing dot is not part of the number");
}

void test_strings_decode_escapes() {
    const auto lexed = lex(R"("a\n\t\\\"\u0041\u{1F600}" '\'' "\uD83D\uDE00")");
    check(lexed.diagnostics.empty(), "valid escapes");
    check(lexed.tokens.size() == 3, "three strings");
    check_eq(lexed.tokens[0].value, "a\n\t\\\"A\xF0\x9F\x98\x80", "decoded escapes");
    check_eq(lexed.tokens[1].value, "'", "single quote escape");
    check_eq(lexed.tokens[2].value, "\xF0\x9F\x98\x80", "surrogate pair combines");
}

void test_invalid_escapes_and_unterminated_strings() {
    const auto escape = lex(R"("a\qb")");
    check_eq(escape.codes(), "TESSERA_JSX_LEX_INVALID_ESCAPE;", "unknown escape");
    check_eq(std::string(escape.diagnostics[0].message), "invalid escape sequence `\\q`",
             "escape message names the sequence");
    check(escape.diagnostics[0].span.span.offset == 2 &&
              escape.diagnostics[0].span.span.length == 2,
          "escape span covers backslash and character");
    check_eq(lex(R"("\uD800")").codes(), "TESSERA_JSX_LEX_INVALID_ESCAPE;", "lone surrogate");
    check_eq(lex(R"("\u{110000}")").codes(), "TESSERA_JSX_LEX_INVALID_ESCAPE;", "beyond Unicode");
    check_eq(lex(R"("\u12")").codes(), "TESSERA_JSX_LEX_INVALID_ESCAPE;", "short \\u escape");

    const auto open = lex("\"abc\nnext");
    check_eq(open.codes(), "TESSERA_JSX_LEX_UNTERMINATED_STRING;", "line break ends a string");
    check(open.diagnostics[0].span.start.line == 1 && open.diagnostics[0].span.start.column == 1,
          "reported at the opening quote");
    check_eq(open.dump(), "str:\"abc id:next ", "lexing continues on the next line");
    check_eq(lex("\"abc").codes(), "TESSERA_JSX_LEX_UNTERMINATED_STRING;", "EOF ends a string");
    check_eq(lex("\"abc\\").codes(), "TESSERA_JSX_LEX_UNTERMINATED_STRING;", "trailing backslash");
}

void test_templates_are_lexed_in_parts() {
    const std::string source = "`a${x}b${y}c` `plain` ";
    SourceMap map(source);
    Limits limits;
    DiagnosticSink sink(map, limits);
    Lexer lexer(source, sink, limits);

    Token head = lexer.next(LexMode::Code);
    check(head.kind == TokenKind::TemplateHead && head.value == "a", "head");
    check(lexer.next(LexMode::Code).text == "x", "substitution token");
    Token close = lexer.next(LexMode::Code);
    check(close.is_punct("}"), "closing brace");
    Token middle = lexer.continue_template(close);
    check(middle.kind == TokenKind::TemplateMiddle && middle.value == "b", "middle");
    check(lexer.next(LexMode::Code).text == "y", "second substitution");
    Token close2 = lexer.next(LexMode::Code);
    Token tail = lexer.continue_template(close2);
    check(tail.kind == TokenKind::TemplateTail && tail.value == "c", "tail");
    Token plain = lexer.next(LexMode::Code);
    check(plain.kind == TokenKind::TemplateString && plain.value == "plain", "no substitutions");
    check(!sink.has_errors(), "no diagnostics");

    check_eq(lex("`abc").codes(), "TESSERA_JSX_LEX_UNTERMINATED_TEMPLATE;", "unterminated");
    check_eq(lex("`a\\`b\\$`").tokens[0].value, "a`b$", "template-only escapes");
    check_eq(lex("`a\r\nb`").tokens[0].value, "a\nb", "CRLF cooks to LF");
}

void test_comments_are_kept_with_spans() {
    const std::string source = "a // line\r\nb /* block\n*/ c";
    const auto lexed = lex(source);
    check_eq(lexed.dump(), "id:a id:b id:c ", "comments are not tokens");
    check(lexed.comments.size() == 2, "two comments");
    check(!lexed.comments[0].block && lexed.comments[0].text == " line", "line comment text");
    check(lexed.comments[0].span.length == 7, "line comment span excludes the CR of CRLF");
    check(lexed.comments[1].block && lexed.comments[1].text == " block\n", "block comment text");
    check_eq(lex("a /* open").codes(), "TESSERA_JSX_LEX_UNTERMINATED_COMMENT;", "unterminated");
}

void test_jsx_tag_mode() {
    check_eq(lex("aria-label=\"x\" data-x", LexMode::JsxTag).dump(),
             "id:aria-label p:= str:\"x\" id:data-x ", "hyphenated names");
    check_eq(lex("<A b={x}/>", LexMode::JsxTag).dump(), "p:< id:A id:b p:= p:{ id:x p:} p:/> ",
             "self-closing tag");
    check_eq(lex("<></>", LexMode::JsxTag).dump(), "p:< p:> p:< p:/> ",
             "fragment closer lexes as < then />");
    check_eq(lex("class if", LexMode::JsxTag).dump(), "id:class id:if ",
             "no keywords inside a tag");
}

void test_jsx_child_mode() {
    const auto lexed = lex("hello  world {<b", LexMode::JsxChild);
    check_eq(lexed.dump(), "text:hello  world  p:{ p:< text:b ", "child text is raw");
    check_eq(lex("a } b", LexMode::JsxChild).codes(), "TESSERA_JSX_LEX_INVALID_TEXT_CHARACTER;",
             "stray } in text");
    check_eq(lex("a > b", LexMode::JsxChild).codes(), "TESSERA_JSX_LEX_INVALID_TEXT_CHARACTER;",
             "stray > in text");
    check_eq(lex("// not a comment", LexMode::JsxChild).dump(), "text:// not a comment ",
             "comments do not exist in child text");
}

void test_source_wide_checks() {
    const auto utf8 = lex(std::string("a \xC3\x28 b \xFF c"));
    check_eq(utf8.codes(), "TESSERA_JSX_LEX_INVALID_UTF8;TESSERA_JSX_LEX_INVALID_UTF8;",
             "invalid UTF-8 reported once per bad byte");
    check(utf8.diagnostics[0].span.span.offset == 2, "first bad byte offset");
    check_eq(lex("\xED\xA0\x80").codes().substr(0, 29), "TESSERA_JSX_LEX_INVALID_UTF8;",
             "encoded surrogate is invalid");
    check_eq(lex("\xC0\x80").codes().substr(0, 29), "TESSERA_JSX_LEX_INVALID_UTF8;",
             "overlong encoding is invalid");

    check_eq(lex("a\rb").codes(), "TESSERA_JSX_LEX_LONE_CR;", "lone CR");
    check(lex("a\r\nb").diagnostics.empty(), "CRLF is fine");

    const auto bom = lex("\xEF\xBB\xBF"
                         "ab");
    check_eq(bom.dump(), "id:ab ", "BOM ignored");
    check(bom.diagnostics.empty(), "BOM is not an error");

    const auto unknown = lex("a # b");
    check_eq(unknown.codes(), "TESSERA_JSX_LEX_INVALID_CHARACTER;", "unknown character");
    check_eq(lex("\xC3\xA9").codes(), "TESSERA_JSX_LEX_INVALID_CHARACTER;",
             "non-ASCII outside strings is not an identifier");
}

void test_source_map_columns_count_code_points() {
    const std::string source = "\xEF\xBB\xBF"
                               "ab\r\n\xC3\xA9\xC3\xA9 x\n";
    SourceMap map(source);
    check(map.locate(3).line == 1 && map.locate(3).column == 1, "BOM does not take a column");
    check(map.locate(4).column == 2, "second column");
    const auto x = map.locate(static_cast<std::uint32_t>(source.find('x')));
    check(x.line == 2 && x.column == 4, "two 2-byte code points then a space: column 4");
    const auto end = map.resolve(Span{3, 2});
    check(end.end.column == 3 && end.end.line == 1, "end of a span");
    check_eq(std::string(map.slice(Span{3, 2})), "ab", "slice");
}

void test_token_spans_slice_the_source() {
    const std::string source = "const a = \"x\\ty\" + 12; // done\n`t${b}`";
    SourceMap map(source);
    Limits limits;
    DiagnosticSink sink(map, limits);
    Lexer lexer(source, sink, limits);
    for (int i = 0; i < 100; ++i) {
        Token token = lexer.next(LexMode::Code);
        if (token.kind == TokenKind::Eof) {
            break;
        }
        check(map.slice(token.span) == token.text, "span slices to the token text");
        if (token.is_punct("}")) {
            Token tail = lexer.continue_template(token);
            check(map.slice(tail.span) == tail.text, "template tail span");
            check(tail.text == "}`", "tail text includes the closing brace");
        }
    }
}

void test_limits() {
    Limits tokens;
    tokens.max_tokens = 3;
    const auto lexed = lex("a b c d e f", LexMode::Code, tokens);
    check(lexed.tokens.size() == 3, "stops at the token limit");
    check_eq(lexed.codes(), "TESSERA_JSX_LIMIT_TOKENS;", "token limit diagnostic reported once");

    Limits size;
    size.max_file_bytes = 4;
    const auto big = lex("abcdefgh", LexMode::Code, size);
    check(big.tokens.empty(), "an oversized file produces no tokens");
    check_eq(big.codes(), "TESSERA_JSX_LIMIT_FILE_SIZE;", "file size diagnostic");

    Limits few;
    few.max_diagnostics = 2;
    const auto noisy = lex("# # # # # #", LexMode::Code, few);
    check_eq(noisy.codes(),
             "TESSERA_JSX_LEX_INVALID_CHARACTER;TESSERA_JSX_LEX_INVALID_CHARACTER;"
             "TESSERA_JSX_LIMIT_DIAGNOSTICS;",
             "diagnostics are truncated with a marker");
    check(noisy.tokens.size() == 6, "lexing continues past the diagnostic limit");
}

void test_every_mode_terminates_on_hostile_bytes() {
    std::string source;
    std::uint32_t state = 12345;
    for (int i = 0; i < 4096; ++i) {
        state = state * 1664525U + 1013904223U;
        source.push_back(static_cast<char>(state >> 24));
    }
    for (const LexMode mode : {LexMode::Code, LexMode::JsxTag, LexMode::JsxChild}) {
        const auto lexed = lex(source, mode);
        std::size_t consumed = 0;
        for (const auto& token : lexed.tokens) {
            check(token.span.length > 0, "every token consumes input");
            consumed += token.span.length;
        }
        check(consumed <= source.size(), "tokens never overlap");
    }
}

} // namespace

int main() {
    test_identifiers_keywords_and_punctuators();
    test_rejected_operators_are_lexed_for_the_parser();
    test_numbers();
    test_strings_decode_escapes();
    test_invalid_escapes_and_unterminated_strings();
    test_templates_are_lexed_in_parts();
    test_comments_are_kept_with_spans();
    test_jsx_tag_mode();
    test_jsx_child_mode();
    test_source_wide_checks();
    test_source_map_columns_count_code_points();
    test_token_spans_slice_the_source();
    test_limits();
    test_every_mode_terminates_on_hostile_bytes();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "jsx lexer tests passed\n";
    return 0;
}
