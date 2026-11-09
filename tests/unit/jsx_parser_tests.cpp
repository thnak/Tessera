// SPDX-License-Identifier: MIT

#include "jsx_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

void test_elements_and_attributes() {
    check_eq(dump_jsx("<View/>"), "(element View/)", "self-closing");
    check_eq(dump_jsx("<View></View>"), "(element View)", "empty element");
    check_eq(dump_jsx("<Button label=\"Go\" disabled onClick={() => go()} {...props}/>"),
             "(element Button/ (attr label \"Go\") (flag disabled) "
             "(attr onClick (arrow () (call (id go)))) (spread (id props)))",
             "string, flag, expression and spread attributes");
    check_eq(dump_jsx("<View aria-label=\"x\" style={{ width: 10 }}/>"),
             "(element View/ (attr aria-label \"x\") (attr style (object (width (num 10)))))",
             "aria- names and object literal values");
    check_eq(dump_jsx("<A\n  b = \"1\"\n  c = { 2 }\n/>"),
             "(element A/ (attr b \"1\") (attr c (num 2)))", "whitespace around =");
    check_eq(dump_jsx("<A b='it\\'s' c=\"&amp;&lt;\"/>"),
             "(element A/ (attr b \"it's\") (attr c \"&<\"))",
             "string escapes and entities in attribute strings");
    check_eq(dump_jsx("<A><B><C/></B><D/></A>"),
             "(element A (element B (element C/)) (element D/))", "nesting");
}

void test_fragments() {
    check_eq(dump_jsx("<><A/><B/></>"), "(fragment (element A/) (element B/))", "fragment");
    check_eq(dump_jsx("<View><><A/></></View>"), "(element View (fragment (element A/)))",
             "fragment as a child");
    check_eq(dump_jsx("<></>"), "(fragment)", "empty fragment");
    check_eq(dump_jsx("<> </>"), "(fragment (text \" \"))", "space-only text without a newline");
}

void test_text_whitespace_rule() {
    check_eq(dump_jsx("<Text>hello</Text>"), "(element Text (text \"hello\"))", "plain text");
    check_eq(dump_jsx("<Text> a </Text>"), "(element Text (text \" a \"))",
             "single-line text keeps its edge spaces");
    check_eq(dump_jsx("<Text>\n    first\n    second\n  </Text>"),
             "(element Text (text \"first second\"))", "lines are trimmed and joined by a space");
    check_eq(dump_jsx("<View>\n  <A/>\n  <B/>\n</View>"),
             "(element View (element A/) (element B/))",
             "blank lines between elements produce no text");
    check_eq(dump_jsx("<Text>a {x} b</Text>"),
             "(element Text (text \"a \") (expr (id x)) (text \" b\"))",
             "text around an expression keeps the spaces next to it");
    check_eq(dump_jsx("<Text>a{\" \"}b</Text>"),
             "(element Text (text \"a\") (expr (str \" \")) (text \"b\"))", "explicit space");
    check_eq(dump_jsx("<Text>a\r\n  b</Text>"), "(element Text (text \"a b\"))", "CRLF lines");
    check_eq(dump_jsx("<Text>a\t\tb</Text>"), "(element Text (text \"a  b\"))",
             "interior tabs become spaces");
    check_eq(dump_jsx("<Text>// not a comment</Text>"),
             "(element Text (text \"// not a comment\"))", "no comments in child text");
}

void test_entities() {
    check_eq(dump_jsx("<T>&amp;&lt;&gt;&quot;&apos;</T>"), "(element T (text \"&<>\\\"'\"))",
             "named entities");
    check_eq(dump_jsx("<T>&#65;&#x42;&#X43;</T>"), "(element T (text \"ABC\"))",
             "numeric entities");
    check_eq(dump_jsx("<T>&nbsp;x</T>"), "(element T (text \"\xC2\xA0x\"))", "nbsp is not trimmed");
    check_eq(dump_jsx("<T>a & b &c &amp</T>"), "(element T (text \"a & b &c &amp\"))",
             "an ampersand that does not start `&name;` is literal");

    const auto unknown = parse_jsx_element("<T>x &foo; y</T>");
    check_eq(codes(unknown), "TESSERA_JSX_PARSE_UNKNOWN_ENTITY;", "unknown entity");
    check(first_diag(unknown).span.span.offset == 5 && first_diag(unknown).span.span.length == 5,
          "entity diagnostic covers `&foo;`");
    check_eq(codes(parse_jsx_element("<T>&#xD800;</T>")), "TESSERA_JSX_PARSE_UNKNOWN_ENTITY;",
             "surrogate reference");
    check_eq(codes(parse_jsx_element("<T>&#0;</T>")), "TESSERA_JSX_PARSE_UNKNOWN_ENTITY;",
             "null reference");
    check_eq(codes(parse_jsx_element("<T a=\"&bogus;\"/>")), "TESSERA_JSX_PARSE_UNKNOWN_ENTITY;",
             "unknown entity in an attribute");
}

void test_expression_children() {
    check_eq(dump_jsx("<V>{cond && <X/>}</V>"),
             "(element V (expr (binary && (id cond) (element X/))))", "conditional element");
    check_eq(dump_jsx("<V>{a ? <X/> : null}</V>"),
             "(element V (expr (cond (id a) (element X/) (null))))", "ternary with null");
    check_eq(dump_jsx("<V>{rows.map((row, i) => (<Row key={row.id}/>))}</V>"),
             "(element V (expr (call (member map (id rows)) (arrow (row i) "
             "(element Row/ (attr key (member id (id row))))))))",
             "list loop");
    check_eq(dump_jsx("<V>{/* note */}</V>"), "(element V (expr))", "comment-only container");
    check_eq(dump_jsx("<V>{}</V>"), "(element V (expr))", "empty container");
    check_eq(dump_jsx("<V>{`a${x}`}</V>"), "(element V (expr (template \"a\" (id x) \"\")))",
             "template in a child");
    check_eq(dump_jsx("<V>{a}{b}</V>"), "(element V (expr (id a)) (expr (id b)))",
             "adjacent containers");
    check_eq(dump_jsx("<V>{<A>t</A>}</V>"), "(element V (expr (element A (text \"t\"))))",
             "element inside a container, closing tag followed by `}`");
}

void test_subset_rejections() {
    const auto html = parse_jsx_element("<div>x</div>");
    check_eq(codes(html), "TESSERA_JSX_SUBSET_HTML_TAG;", "lowercase tag");
    check(html.root != nullptr && !html.ok(), "tree kept but not ok");
    check_eq(first_diag(html).suggestion, "use `<View>`", "suggests the intrinsic");
    check_eq(format_diagnostic(first_diag(html)),
             "1:2: error[TESSERA_JSX_SUBSET_HTML_TAG]: HTML tag `<div>` is not supported\n"
             "  help: use `<View>`",
             "html tag diagnostic text");
    check_eq(first_diag(parse_jsx_element("<foo/>")).suggestion,
             "tags start with an uppercase letter, for example `<Foo>`", "generic suggestion");

    check_eq(codes(parse_jsx_element("<A.B/>")), "TESSERA_JSX_SUBSET_DYNAMIC_TAG;", "member tag");
    check_eq(codes(parse_jsx_element("<a:b/>")), "TESSERA_JSX_SUBSET_DYNAMIC_TAG;",
             "namespace tag");
    check_eq(codes(parse_jsx_element("<A data-x=\"1\"/>")), "TESSERA_JSX_SUBSET_UNKNOWN_PROP;",
             "hyphen outside aria-");
    check_eq(codes(parse_jsx_element("<A xlink:href=\"1\"/>")), "TESSERA_JSX_SUBSET_UNKNOWN_PROP;",
             "namespaced attribute");
    check_eq(codes(parse_jsx_element("<A dangerouslySetInnerHTML={x}/>")),
             "TESSERA_JSX_SUBSET_DOM;", "dangerouslySetInnerHTML");
    check_eq(codes(parse_jsx_element("<A b={x == y}/>")), "TESSERA_JSX_SUBSET_LOOSE_EQUALITY;",
             "expression rejections surface from attributes");
    check_eq(codes(parse_jsx_element("<div><span/></div>")),
             "TESSERA_JSX_SUBSET_HTML_TAG;TESSERA_JSX_SUBSET_HTML_TAG;",
             "independent errors are all reported, in source order");
}

void test_syntax_errors() {
    const auto mismatch = parse_jsx_element("<A>\n  <B>x</C>\n</A>");
    check_eq(codes(mismatch), "TESSERA_JSX_PARSE_MISMATCHED_TAG;", "mismatched closing tag");
    check(mismatch.root != nullptr, "the structure is clear, so the tree is kept");
    check_eq(format_diagnostic(first_diag(mismatch)),
             "2:7: error[TESSERA_JSX_PARSE_MISMATCHED_TAG]: closing tag `</C>` does not match "
             "`<B>`\n  help: close it with `</B>`\n  note: 2:4: `<B>` is opened here",
             "mismatch text names both tags and points at the opening tag");

    check_eq(codes(parse_jsx_element("<A></>")), "TESSERA_JSX_PARSE_MISMATCHED_TAG;",
             "fragment closer on an element");
    check_eq(codes(parse_jsx_element("<></A>")), "TESSERA_JSX_PARSE_MISMATCHED_TAG;",
             "element closer on a fragment");
    check_eq(codes(parse_jsx_element("<A>text")), "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT;",
             "unclosed element");
    check_eq(codes(parse_jsx_element("<A><B>")), "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT;",
             "unclosed nested element reports once, at the innermost");
    check_eq(codes(parse_jsx_element("<A b=\"1\"")), "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT;",
             "unclosed tag");
    check_eq(codes(parse_jsx_element("<A b=1/>")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "bare attribute value");
    check_eq(codes(parse_jsx_element("<A b=<C/>/>")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "element as attribute value");
    check_eq(codes(parse_jsx_element("<A b={}/>")), "TESSERA_JSX_PARSE_EMPTY_EXPRESSION;",
             "empty attribute expression");
    check_eq(codes(parse_jsx_element("<A {b}/>")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "braces that are not a spread");
    check_eq(codes(parse_jsx_element("<A/> x")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "input after the element");
    check_eq(codes(parse_jsx_element("x")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "not an element");
    check_eq(codes(parse_jsx_element("")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "empty input");
    check_eq(codes(parse_jsx_element("<A>{a b}</A>")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "two expressions in a container");
    check_eq(codes(parse_jsx_element("<A>a } b</A>")), "TESSERA_JSX_LEX_INVALID_TEXT_CHARACTER;",
             "stray brace in text");
    check_eq(codes(parse_jsx_element("<A>{...x}</A>")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "spread child");
    check_eq(codes(parse_jsx_element("<>")), "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT;",
             "open fragment");
    check_eq(codes(parse_jsx_element("<")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "lone <");
}

void test_comments() {
    const auto result = parse_jsx_element("<A // first\n  b=\"x\" /* second */ />");
    check(result.ok(), "comments inside a tag are trivia");
    check(result.comments.size() == 2, "both comments recorded");
    check_eq(std::string(result.comments[0].text), " first", "line comment");
    check(result.comments[1].block, "block comment");
}

void test_spans() {
    const std::string source = "<View a=\"1\">hi{x}<B/></View>";
    const auto result = parse_jsx_element(source);
    check(result.ok(), "parses");
    if (!result.root) {
        return;
    }
    const SourceMap map(source);
    const JsxNode& root = *result.root;
    check_eq(std::string(map.slice(root.span)), source, "element covers open to close tag");
    check_eq(std::string(map.slice(root.tag_span)), "View", "tag name span");
    check_eq(std::string(map.slice(root.attributes[0].span)), "a=\"1\"", "attribute span");
    check_eq(std::string(map.slice(root.attributes[0].name_span)), "a", "attribute name span");
    check_eq(std::string(map.slice(root.children[0]->span)), "hi", "text span");
    check_eq(std::string(map.slice(root.children[1]->span)), "{x}", "container span");
    check_eq(std::string(map.slice(root.children[2]->span)), "<B/>", "self-closing child span");

    const std::string fragment = "<>a</>";
    const auto fragment_result = parse_jsx_element(fragment);
    if (!fragment_result.root) {
        check(false, "fragment parses");
        return;
    }
    check_eq(std::string(SourceMap(fragment).slice(fragment_result.root->span)), fragment,
             "fragment span");
}

void test_locations_with_crlf_and_non_ascii() {
    const std::string source = "<A>\r\n  \xC3\xA9\xC3\xA9 <b/>\r\n</A>";
    const auto result = parse_jsx_element(source);
    check_eq(codes(result), "TESSERA_JSX_SUBSET_HTML_TAG;", "lowercase child tag");
    const auto& where = first_diag(result).span;
    check(where.start.line == 2 && where.start.column == 7,
          "line 2, column 7 counts the two 2-byte code points and the space after the indent");
    check(where.end.line == 2 && where.end.column == 8, "end column");
    check(where.span.length == 1, "byte length of `b`");
}

void test_determinism() {
    const std::string source =
        "<View a=\"1\">\n  {rows.map((r) => (<Row key={r.id}/>))}\n  <div/>\n</View>";
    const auto first = parse_jsx_element(source);
    if (!first.root || first.diagnostics.empty()) {
        check(false, "the determinism sample parses with its diagnostic");
        return;
    }
    const auto second = parse_jsx_element(source);
    if (!second.root) {
        check(false, "second parse has a tree");
        return;
    }
    check_eq(dump(*first.root), dump(*second.root), "same tree");
    check_eq(codes(first), codes(second), "same diagnostics");
    check(first_diag(first).span.span.offset == first_diag(second).span.span.offset, "same spans");
}

} // namespace

int main() {
    test_elements_and_attributes();
    test_fragments();
    test_text_whitespace_rule();
    test_entities();
    test_expression_children();
    test_subset_rejections();
    test_syntax_errors();
    test_comments();
    test_spans();
    test_locations_with_crlf_and_non_ascii();
    test_determinism();
    return finish("jsx parser tests");
}
