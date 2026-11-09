// SPDX-License-Identifier: MIT

#include "jsx_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

void test_precedence_and_associativity() {
    check_eq(dump_expr("1 + 2 * 3"), "(binary + (num 1) (binary * (num 2) (num 3)))",
             "* binds tighter than +");
    check_eq(dump_expr("a - b - c"), "(binary - (binary - (id a) (id b)) (id c))",
             "left associative");
    check_eq(dump_expr("a || b && c"), "(binary || (id a) (binary && (id b) (id c)))",
             "&& binds tighter than ||");
    check_eq(dump_expr("a === b && c < d"),
             "(binary && (binary === (id a) (id b)) (binary < (id c) (id d)))",
             "equality and relational levels");
    check_eq(dump_expr("a < b + c"), "(binary < (id a) (binary + (id b) (id c)))",
             "additive binds tighter than relational");
    check_eq(dump_expr("(a + b) * c"), "(binary * (binary + (id a) (id b)) (id c))",
             "parentheses override precedence");
    check_eq(dump_expr("a % 2 !== 0"), "(binary !== (binary % (id a) (num 2)) (num 0))",
             "modulo then inequality");
}

void test_unary_conditional_member_call() {
    check_eq(dump_expr("!a"), "(unary ! (id a))", "not");
    check_eq(dump_expr("- -a"), "(unary - (unary - (id a)))", "nested unary");
    check_eq(dump_expr("-a * b"), "(binary * (unary - (id a)) (id b))", "unary binds tighter");
    check_eq(dump_expr("a ? b : c ? d : e"), "(cond (id a) (id b) (cond (id c) (id d) (id e)))",
             "conditional nests to the right");
    check_eq(dump_expr("a || b ? c : d"), "(cond (binary || (id a) (id b)) (id c) (id d))",
             "test is a full || expression");
    check_eq(dump_expr("a.b.c"), "(member c (member b (id a)))", "member chain");
    check_eq(dump_expr("f(1, x)"), "(call (id f) (num 1) (id x))", "call");
    check_eq(dump_expr("f()"), "(call (id f))", "empty call");
    check_eq(dump_expr("f(a,)"), "(call (id f) (id a))", "trailing comma in a call");
    check_eq(dump_expr("a.b(c).d"), "(member d (call (member b (id a)) (id c)))",
             "call then member");
    check_eq(dump_expr("row.type"), "(member type (id row))", "a keyword is a valid member name");
}

void test_literals() {
    check_eq(dump_expr("42"), "(num 42)", "number");
    check_eq(dump_expr("\"a\\nb\""), "(str \"a\\nb\")", "string with escape");
    check_eq(dump_expr("true"), "(true)", "true");
    check_eq(dump_expr("false"), "(false)", "false");
    check_eq(dump_expr("null"), "(null)", "null");
    check_eq(dump_expr("`plain`"), "(template \"plain\")", "template without substitutions");
    check_eq(dump_expr("`a${x}b${y + 1}c`"),
             "(template \"a\" (id x) \"b\" (binary + (id y) (num 1)) \"c\")",
             "template with substitutions");
    check_eq(dump_expr("`${`in${x}`}`"), "(template \"\" (template \"in\" (id x) \"\") \"\")",
             "nested templates");
    check_eq(dump_expr("[1, a, \"s\"]"), "(array (num 1) (id a) (str \"s\"))", "array");
    check_eq(dump_expr("[]"), "(array)", "empty array");
    check_eq(dump_expr("{ a: 1, \"b-c\": x, d }"),
             "(object (a (num 1)) (\"b-c\" (id x)) (d (id d)))",
             "object literal with string key and shorthand");
    check_eq(dump_expr("{ type: 1 }"), "(object (type (num 1)))", "keyword key");
}

void test_arrow_functions() {
    check_eq(dump_expr("x => x + 1"), "(arrow (x) (binary + (id x) (num 1)))", "single parameter");
    check_eq(dump_expr("(a, b) => a"), "(arrow (a b) (id a))", "two parameters");
    check_eq(dump_expr("() => setCount(count + 1)"),
             "(arrow () (call (id setCount) (binary + (id count) (num 1))))", "no parameters");
    check_eq(dump_expr("() => { setA(1); setB(2); }"),
             "(arrow () (block (call (id setA) (num 1)) (call (id setB) (num 2))))",
             "block body of calls");
    check_eq(dump_expr("() => {}"), "(arrow () (block))", "empty block");
    check_eq(dump_expr("(a)"), "(id a)", "a parenthesized identifier is not an arrow");
    check_eq(dump_expr("(a) * 2"), "(binary * (id a) (num 2))", "paren then operator");
    check_eq(dump_expr("(row, i) => (row.id)"), "(arrow (row i) (member id (id row)))",
             "parenthesized body");
    check_eq(dump_expr("(a,)=> a"), "(arrow (a) (id a))", "trailing comma in parameters");

    const auto bad = parse_expression("(a, b)");
    check(bad.root == nullptr, "a parenthesized list is not an expression");
    check_eq(codes(bad), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "reported once");
}

void test_speculation_leaves_no_diagnostics_behind() {
    // The arrow-parameter attempt lexes `#` and reports it; the real parse stops at `,` first, so
    // the attempt's diagnostic must have been rewound.
    const auto result = parse_expression("(a, # b)");
    check_eq(codes(result), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "diagnostics from the abandoned attempt are dropped");
}

void test_subset_rejections_keep_parsing() {
    struct Case {
        const char* source;
        const char* code;
    };
    const Case cases[] = {
        {"a == b", "TESSERA_JSX_SUBSET_LOOSE_EQUALITY;"},
        {"a != b", "TESSERA_JSX_SUBSET_LOOSE_EQUALITY;"},
        {"a ?? b", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"a?.b", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"a ** b", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"a & b", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"a << b", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"~a", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"typeof a", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"a in b", "TESSERA_JSX_SUBSET_OPERATOR;"},
        {"i++", "TESSERA_JSX_SUBSET_MUTATION;"},
        {"--i", "TESSERA_JSX_SUBSET_MUTATION;"},
        {"x = 1", "TESSERA_JSX_SUBSET_MUTATION;"},
        {"x += 1", "TESSERA_JSX_SUBSET_MUTATION;"},
        {"a[0]", "TESSERA_JSX_SUBSET_MEMBER;"},
        {"await x", "TESSERA_JSX_SUBSET_ASYNC;"},
    };
    for (const auto& test : cases) {
        const auto result = parse_expression(test.source);
        check_eq(codes(result), test.code, std::string("rejection of ") + test.source);
        check(result.root != nullptr, std::string("tree kept for ") + test.source);
        check(!result.ok(), std::string("not ok: ") + test.source);
    }
    check_eq(dump_expr("a == b"), "(binary == (id a) (id b))", "operator spelling is preserved");

    const auto two = parse_expression("a == b != c");
    check_eq(codes(two), "TESSERA_JSX_SUBSET_LOOSE_EQUALITY;TESSERA_JSX_SUBSET_LOOSE_EQUALITY;",
             "every independent error is reported");
}

void test_rejected_keywords_are_fatal_and_specific() {
    struct Case {
        const char* source;
        const char* code;
    };
    const Case cases[] = {
        {"class A {}", "TESSERA_JSX_SUBSET_CLASS;"},
        {"new X()", "TESSERA_JSX_SUBSET_CLASS;"},
        {"this", "TESSERA_JSX_SUBSET_CLASS;"},
        {"let x", "TESSERA_JSX_SUBSET_MUTATION;"},
        {"if (a) {}", "TESSERA_JSX_SUBSET_STATEMENT;"},
        {"() => { return 1; }", "TESSERA_JSX_SUBSET_STATEMENT;"},
        {"async () => 1", "TESSERA_JSX_SUBSET_ASYNC;"},
        {"function () {}", "TESSERA_JSX_SUBSET_FUNCTION_VALUE;"},
        {"import x", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"{ [k]: 1 }", "TESSERA_JSX_SUBSET_LITERAL;"},
        {"{ ...a }", "TESSERA_JSX_SUBSET_LITERAL;"},
    };
    for (const auto& test : cases) {
        const auto result = parse_expression(test.source);
        check_eq(codes(result), test.code, std::string("keyword rejection of ") + test.source);
        check(result.root == nullptr, std::string("no tree for ") + test.source);
    }
}

void test_syntax_errors() {
    check_eq(codes(parse_expression("")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "empty input");
    check_eq(codes(parse_expression("a +")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "dangling +");
    check_eq(codes(parse_expression("a b")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "two expressions");
    check_eq(codes(parse_expression("f(1")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "open call");
    check_eq(codes(parse_expression("a ? b")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "conditional without else");
    check_eq(codes(parse_expression("a.")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "member name");
    check_eq(codes(parse_expression("`a${x")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "substitution without a closing brace");
    check_eq(codes(parse_expression("`a${x}b")), "TESSERA_JSX_LEX_UNTERMINATED_TEMPLATE;",
             "unterminated template tail is a lexer diagnostic");

    const auto result = parse_expression("a +\n  )");
    check(result.diagnostics.size() == 1, "one diagnostic");
    check_eq(format_diagnostic(first_diag(result)),
             "2:3: error[TESSERA_JSX_PARSE_UNEXPECTED_TOKEN]: unexpected `)`, expected an "
             "expression",
             "diagnostic text and location");
}

void test_diagnostic_text_golden() {
    const auto result = parse_expression("a == b");
    check_eq(format_diagnostic(first_diag(result)),
             "1:3: error[TESSERA_JSX_SUBSET_LOOSE_EQUALITY]: loose equality is not supported\n"
             "  help: use `===`",
             "loose equality text");
}

void test_node_spans_slice_the_source() {
    const std::string source = "(a + b) * c(d, \"s\")";
    const auto result = parse_expression(source);
    check(result.ok(), "parses");
    if (!result.root) {
        return;
    }
    const SourceMap map(source);
    const Expr& root = *result.root;
    check_eq(std::string(map.slice(root.span)), source, "root covers the whole expression");
    check_eq(std::string(map.slice(root.operands[0]->span)), "(a + b)",
             "a parenthesized node covers its parentheses");
    check_eq(std::string(map.slice(root.operands[1]->span)), "c(d, \"s\")", "call span");
    check_eq(std::string(map.slice(root.operands[1]->operands[2]->span)), "\"s\"", "string span");
}

void test_comments_are_returned() {
    const auto result = parse_expression("a /* one */ + // two\n b");
    check(result.ok(), "comments are trivia");
    check(result.comments.size() == 2, "both comments kept");
    check_eq(std::string(result.comments[1].text), " two", "line comment text");
}

} // namespace

int main() {
    test_precedence_and_associativity();
    test_unary_conditional_member_call();
    test_literals();
    test_arrow_functions();
    test_speculation_leaves_no_diagnostics_behind();
    test_subset_rejections_keep_parsing();
    test_rejected_keywords_are_fatal_and_specific();
    test_syntax_errors();
    test_diagnostic_text_golden();
    test_node_spans_slice_the_source();
    test_comments_are_returned();
    return finish("jsx expression tests");
}
