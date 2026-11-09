// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/lexer.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::compiler::jsx {

// Recursive-descent parser over the parser-driven Lexer. It holds one token of lookahead (`cur_`)
// that was lexed in the mode the previous advance() asked for, so every consumption site states
// the mode of the token that follows (see LexMode).
//
// Error model: fail() reports an error and sets `fatal_`; every loop and every parse function
// checks it and unwinds, so the parser stops at the first structural error. report() records a
// subset violation and parsing continues.
class Parser {
public:
    Parser(std::string_view source, const Limits& limits);

    JsxPtr parse_jsx_root();
    ExprPtr parse_expression_root();

    template<typename Node> ParseResult<Node> finish(std::unique_ptr<Node> root) {
        ParseResult<Node> result;
        result.root = std::move(root);
        result.comments = lexer_.take_comments();
        result.diagnostics_truncated = sink_.truncated();
        result.diagnostics = sink_.take();
        return result;
    }

private:
    struct Checkpoint {
        Lexer::Mark lexer;
        DiagnosticSink::Mark sink;
        Token cur;
        std::uint32_t prev_end = 0;
    };

    // Counts one level of parser recursion; entered() is false (and the parse failed with
    // TESSERA_JSX_LIMIT_DEPTH) when the limit was hit.
    class DepthScope {
    public:
        explicit DepthScope(Parser& parser)
            : parser_(parser),
              entered_(parser.enter()) {}
        ~DepthScope() {
            if (entered_) {
                parser_.leave();
            }
        }
        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;
        [[nodiscard]] bool entered() const {
            return entered_;
        }

    private:
        Parser& parser_;
        bool entered_;
    };

    // parser.cpp
    void advance(LexMode mode);
    void advance_template();
    [[nodiscard]] Checkpoint checkpoint() const;
    void restore(Checkpoint saved);
    [[nodiscard]] bool at_punct(std::string_view punctuator) const {
        return cur_.is_punct(punctuator);
    }
    [[nodiscard]] Span span_from(std::uint32_t begin) const {
        return Span{begin, prev_end_ - begin};
    }
    void fail(std::string_view code, std::string message, Span span, std::string suggestion = {});
    void fail_unexpected(std::string_view expected);
    void report(std::string_view code, std::string message, Span span, std::string suggestion = {});
    bool expect_punct(std::string_view punctuator, LexMode after);
    [[nodiscard]] bool enter();
    void leave() {
        --depth_;
    }
    [[nodiscard]] bool finish_node(Expr& expr);
    [[nodiscard]] bool finish_node(JsxNode& node);
    [[nodiscard]] std::string describe_current() const;

    // parser_expr.cpp
    ExprPtr parse_expr();
    ExprPtr parse_conditional();
    ExprPtr parse_binary(int min_precedence);
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr make_expr(ExprKind kind, std::uint32_t begin);
    ExprPtr make_operator(ExprKind kind, std::uint32_t begin, std::string op,
                          std::vector<ExprPtr> operands);

    // parser_primary.cpp
    ExprPtr parse_primary();
    ExprPtr parse_parenthesized_or_arrow();
    ExprPtr parse_arrow(std::uint32_t begin, std::vector<std::string> params);
    ExprPtr parse_template();
    ExprPtr parse_array();
    ExprPtr parse_object();
    ExprPtr parse_rejected_keyword();

    // parser_jsx.cpp
    ExprPtr parse_jsx_expression();
    JsxPtr parse_jsx_element(LexMode after);
    JsxPtr parse_jsx_after_lt(std::uint32_t begin, LexMode after);
    JsxPtr parse_fragment(std::uint32_t begin, LexMode after);
    JsxPtr parse_named_element(std::uint32_t begin, LexMode after);
    bool parse_tag_name(std::string& name, Span& span);
    bool parse_attributes(JsxNode& element);
    bool parse_attribute(JsxNode& element);
    bool parse_attribute_value(JsxAttribute& attribute);
    bool parse_children(JsxNode& parent, Span opening);
    bool parse_closing_tag(const JsxNode& element, Span opening, LexMode after);
    void add_text_child(JsxNode& parent);
    void add_container_child(JsxNode& parent);
    void check_tag_name(const std::string& name, Span span);

    Limits limits_;
    SourceMap map_;
    DiagnosticSink sink_;
    Lexer lexer_;
    Token cur_;
    std::uint32_t prev_end_ = 0;
    Span closing_lt_; // the `<` that started the closing tag found by parse_children
    std::size_t depth_ = 0;
    bool fatal_ = false;
};

} // namespace tessera::compiler::jsx
