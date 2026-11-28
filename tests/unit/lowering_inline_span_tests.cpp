// SPDX-License-Identifier: MIT

// Inline spans (issue #229, elements-basic.md section 2.1): a Text may contain Text children that
// become attributed runs, the emphasis aliases map to spans with injected default styles, and the
// V1 limits are reported as first errors. Issue #592 (ADR-039): a lowercase alias is resolved
// wherever it is written, so the same table also maps a tag written outside a `Text`, and a tag
// with no mapping keeps TESSERA_JSX_SUBSET_HTML_TAG from the lowering lookup.

#include "lowering_nodes_test_util.hpp"

#include <string>

namespace {

using namespace lowering_nodes_test;

const ir::Node* child_at(const ir::Node& node, std::size_t index) {
    return index < node.children.size() ? &node.children[index] : nullptr;
}

void test_link_span() {
    const auto fixture = parse(component(
        "  const [count, setCount] = useState<int>(0);\n",
        "<Text>By continuing you agree to the <a onClick={() => setCount(count + 1)}>Terms</a>."
        "</Text>",
        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a link span lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check_eq(std::string(node_kind(root)), "Text", "the root is Text");
    check(!root.text, "a Text with spans has no text of its own");
    check_eq(root.children.size() == 3 ? std::string("3") : std::string("?"), "3",
             "three runs: text, link span, text");
    if (root.children.size() != 3) {
        return;
    }
    check_eq(std::string(node_kind(root.children[0])), "Text", "the first run is a span");
    check_eq(expr_text(root.children[0].text.get()),
             "{\"kind\":\"literal\",\"value\":\"By continuing you agree to the \"}",
             "the leading run text");
    check_eq(expr_text(root.children[1].text.get()), "{\"kind\":\"literal\",\"value\":\"Terms\"}",
             "the link span text");
    check_eq(expr_text(root.children[1].props.at("role").get()),
             "{\"kind\":\"literal\",\"value\":\"link\"}", "the a alias injects role=link");
    check(root.children[1].events.count("onClick") == 1, "the link span carries onClick");
    check_eq(expr_text(root.children[2].text.get()), "{\"kind\":\"literal\",\"value\":\".\"}",
             "the trailing run text");
    check(root.children[0].id == "node::2" && root.children[1].id == "node::3" &&
              root.children[2].id == "node::4",
          "span ids are pre-order");
}

void test_emphasis_defaults() {
    const auto fixture = parse(component(
        "", "<Text>Hello <b>bold</b> <i>it</i> <code>x</code> <strong>s</strong> <em>e</em></Text>",
        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "emphasis spans lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(!root.text, "the parent has no text");
    check(root.children.size() == 10, "five spans and five synthetic runs");
    if (root.children.size() != 10) {
        return;
    }
    const ir::Node* bold = child_at(root, 1);
    const ir::Node* italic = child_at(root, 3);
    const ir::Node* code = child_at(root, 5);
    const ir::Node* strong = child_at(root, 7);
    const ir::Node* em = child_at(root, 9);
    check(bold != nullptr && bold->style.count("font-weight") == 1 &&
              expr_text(bold->style.at("font-weight").get()) ==
                  "{\"kind\":\"literal\",\"value\":\"700\"}",
          "b injects font-weight: 700");
    check(italic != nullptr && italic->style.count("font-style") == 1 &&
              expr_text(italic->style.at("font-style").get()) ==
                  "{\"kind\":\"literal\",\"value\":\"italic\"}",
          "i injects font-style: italic");
    check(code != nullptr && code->style.count("font-family") == 1 &&
              expr_text(code->style.at("font-family").get()) ==
                  "{\"kind\":\"literal\",\"value\":\"monospace\"}",
          "code injects font-family: monospace (inert in V1)");
    check(strong != nullptr && strong->style.count("font-weight") == 1, "strong injects weight");
    check(em != nullptr && em->style.count("font-style") == 1, "em injects italic");
    check_eq(expr_text(bold->text.get()), "{\"kind\":\"literal\",\"value\":\"bold\"}",
             "the b span text");
}

void test_explicit_style_wins() {
    const auto fixture = parse(component(
        "", "<Text><b style={{ fontWeight: 500, color: \"red\" }} className=\"b\">x</b></Text>",
        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "an explicit span style lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node* span = child_at(result->component.root, 0);
    check(span != nullptr && expr_text(span->style.at("font-weight").get()) ==
                                 "{\"kind\":\"literal\",\"value\":500}",
          "an explicit font-weight wins over the injected default");
    check(span != nullptr && span->style.count("color") == 1, "the explicit colour is kept");
    check(span != nullptr && span->props.count("className") == 1, "the span className is kept");
}

void test_intrinsic_text_span() {
    const auto fixture =
        parse(component("", "<Text>a <Text className=\"em\">b</Text> c</Text>", "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "an intrinsic Text span lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node* span = child_at(result->component.root, 1);
    check(span != nullptr && span->kind == ir::NodeKind::Text, "the span is a Text node");
    check(span != nullptr && expr_text(span->props.at("className").get()) ==
                                 "{\"kind\":\"literal\",\"value\":\"em\"}",
          "the span className lowers");
}

void test_rejections() {
    {
        const auto fixture = parse(component("  const [count, setCount] = useState<int>(0);\n",
                                             "<Text>a {count} <b>b</b></Text>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_JSX_SPAN_DYNAMIC",
              "dynamic text inside a spanned Text is rejected");
    }
    {
        const auto fixture =
            parse(component("", "<Text>a <b><i>nested</i></b></Text>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_JSX_SPAN_NESTED",
              "a nested span is rejected");
    }
    {
        const auto fixture =
            parse(component("", "<Text><b style={{ fontSize: 12 }}>x</b></Text>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_JSX_SPAN_METRIC",
              "a metric style override on a span is rejected");
    }
    {
        const auto fixture = parse(component(
            "  const [count, setCount] = useState<int>(0);\n",
            "<Text><b onPointerDown={() => setCount(count + 1)}>x</b></Text>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_JSX_SPAN_ATTRIBUTE",
              "an event other than onClick on a span is rejected");
    }
    {
        const auto fixture = parse(component("", "<Text>a <View /> b</Text>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_JSX_CHILDREN_NOT_ALLOWED",
              "a non-span element child is rejected");
    }
    {
        std::string spans;
        for (int index = 0; index < 17; ++index) {
            spans += "<b>x</b>";
        }
        const auto fixture = parse(component("", "<Text>" + spans + "</Text>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_JSX_SPAN_LIMIT",
              "more than 16 spans is rejected");
    }
    {
        std::string text(300, 'x');
        const auto fixture = parse(component("", "<Text>" + text + " <b>y</b></Text>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_JSX_SPAN_LIMIT",
              "more than 256 span text bytes is rejected");
    }
}

// Issue #592 (ADR-039): a lowercase alias is not limited to a `Text` child. Outside a span it is
// the plain intrinsic, and the emphasis defaults of html-aliases.md section 2 still apply.
void test_alias_outside_span() {
    const auto fixture = parse(component("", "<b>x</b>", "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "an alias outside a Text span lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check_eq(std::string(node_kind(root)), "Text", "b outside a span is a Text element");
    check(root.style.count("font-weight") == 1, "the b default font-weight still applies");
    check_eq(expr_text(root.text.get()), "{\"kind\":\"literal\",\"value\":\"x\"}",
             "its text is the run");
}

// Issue #592: the parser accepts every lowercase tag, so TESSERA_JSX_SUBSET_HTML_TAG now comes from
// the lowering alias lookup (schema_bridge.cpp): a tag in neither the element nor the alias table
// gets the generic alternative, a section-5 row gets its own Alternative text.
void test_unmapped_tag_diagnostic() {
    const auto unknown = parse(component("", "<foo/>", "props: P"));
    const auto unknown_result = lower(*unknown);
    check(!unknown_result.has_value(), "a tag with no mapping is refused at lowering");
    if (unknown_result || unknown_result.error().empty()) {
        return;
    }
    const jsx::Diagnostic& unmapped = unknown_result.error().front();
    check_eq(std::string(unmapped.code), "TESSERA_JSX_SUBSET_HTML_TAG", "the unknown-tag code");
    check_eq(unmapped.message, "HTML tag `<foo>` is not supported", "the unknown-tag message");
    check_eq(unmapped.suggestion, "use an intrinsic such as `<View>` or `<Text>`",
             "the generic alternative");
    check_eq(std::string(unknown->map.slice(unmapped.span.span)), "foo", "the tag-name span");

    const auto section_five = parse(component("", "<iframe/>", "props: P"));
    const auto section_result = lower(*section_five);
    check(!section_result.has_value(), "a section-5 tag keeps its refusal");
    if (section_result || section_result.error().empty()) {
        return;
    }
    const jsx::Diagnostic& unsupported = section_result.error().front();
    check_eq(std::string(unsupported.code), "TESSERA_JSX_SUBSET_HTML_TAG", "the section-5 code");
    check_eq(unsupported.suggestion, "`CustomElement`", "the row's Alternative is the suggestion");
    check_eq(std::string(section_five->map.slice(unsupported.span.span)), "iframe", "the span");
}

// Issue #592 acceptance: every mapped tag of html-aliases.md section 2 compiles to its carrier, the
// AST keeps the tag as written (rule 7), and the IR keeps its closed node kinds (ADR-039, ADR-059).
struct AliasCase {
    const char* tag;     // as written in the source
    const char* source;  // the returned element
    const char* carrier; // the closed IR kind
    const char* role;    // injected role, empty when the row injects none
};

constexpr AliasCase kAliasCases[] = {
    {"div", "<div>hi</div>", "View", ""},
    {"span", "<span>hi</span>", "Text", ""},
    {"p", "<p>hi</p>", "Text", "paragraph"},
    {"h1", "<h1>hi</h1>", "Text", "heading"},
    {"h2", "<h2>hi</h2>", "Text", "heading"},
    {"h3", "<h3>hi</h3>", "Text", "heading"},
    {"section", "<section><div/></section>", "View", ""},
    {"main", "<main/>", "View", ""},
    {"label", "<label>hi</label>", "Text", ""},
    {"button", "<button>Go</button>", "Button", ""},
    {"img", "<img src=\"a.png\" alt=\"x\"/>", "Image", ""},
    {"input", "<input/>", "TextInput", ""},
    {"textarea", "<textarea/>", "TextArea", ""},
};

void test_alias_tag_conformance() {
    for (const AliasCase& test : kAliasCases) {
        const std::string where = std::string("`<") + test.tag + ">`: ";
        const auto fixture = parse(component("", test.source, "props: P"));
        const auto result = lower(*fixture);
        check(result.has_value(), where + "lowers: " + describe_errors(*fixture, result));
        if (!result) {
            continue;
        }
        const ir::Node& root = result->component.root;
        check_eq(std::string(node_kind(root)), test.carrier, where + "carrier");
        const rules::ComponentAnalysis* analysis = analysis_of(*fixture);
        check(analysis != nullptr && analysis->item != nullptr &&
                  analysis->item->returned != nullptr && analysis->item->returned->tag == test.tag,
              where + "the AST keeps the tag as written");
        if (test.role[0] == '\0') {
            check(root.props.count("role") == 0, where + "no role is injected");
        } else {
            check(root.props.count("role") == 1 &&
                      expr_text(root.props.at("role").get()) ==
                          "{\"kind\":\"literal\",\"value\":\"" + std::string(test.role) + "\"}",
                  where + "the row's role is injected");
        }
    }
}

} // namespace

// ADR-059 group A (issue #541): the text and break tags are mapped, each with its injected default.
std::string style_of(const ir::Node& node, const char* property) {
    const auto found = node.style.find(property);
    return found == node.style.end() ? std::string("<unset>") : expr_text(found->second.get());
}

std::string literal(const char* value) {
    return std::string("{\"kind\":\"literal\",\"value\":\"") + value + "\"}";
}

void test_group_a_spans() {
    struct Case {
        const char* tag;
        const char* property;
        const char* value;
    };
    constexpr Case kCases[] = {
        {"u", "text-decoration-line", "underline"}, {"s", "text-decoration-line", "line-through"},
        {"mark", "background-color", "#ffff00"},    {"mark", "color", "#000000"},
        {"kbd", "font-family", "monospace"},        {"cite", "font-style", "italic"},
        {"sub", "vertical-align", "sub"},           {"sup", "vertical-align", "super"},
        {"small", "font-size", "0.83em"},
    };
    for (const Case& test : kCases) {
        const std::string where = std::string("`<") + test.tag + ">`: ";
        const std::string source =
            std::string("<Text>a <") + test.tag + ">x</" + test.tag + "></Text>";
        const auto fixture = parse(component("", source, "props: P"));
        const auto result = lower(*fixture);
        check(result.has_value(),
              where + "lowers inside a Text: " + describe_errors(*fixture, result));
        if (!result || result->component.root.children.size() != 2) {
            check(false, where + "has two runs");
            continue;
        }
        const ir::Node& span = result->component.root.children[1];
        check_eq(style_of(span, test.property), literal(test.value), where + "injected default");
        check_eq(expr_text(span.props.at("tag").get()), literal(test.tag), where + "keeps its tag");
    }
    const auto abbr = parse(component("", "<Text>a <abbr>x</abbr></Text>", "props: P"));
    const auto abbr_result = lower(*abbr);
    check(abbr_result.has_value(), "abbr lowers: " + describe_errors(*abbr, abbr_result));
}

void test_explicit_decoration_wins() {
    const auto fixture = parse(component(
        "", "<Text>a <u style={{ textDecorationLine: \"none\" }}>x</u></Text>", "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "an authored decoration on u lowers: " + describe_errors(*fixture, result));
}

void test_break_and_rule() {
    const auto in_text = parse(component("", "<Text>a<br />b</Text>", "props: P"));
    const auto in_text_result = lower(*in_text);
    check(in_text_result.has_value(),
          "br inside a Text lowers: " + describe_errors(*in_text, in_text_result));
    if (in_text_result && in_text_result->component.root.children.size() == 3) {
        const ir::Node& br = in_text_result->component.root.children[1];
        check_eq(expr_text(br.text.get()), literal("\\n"), "a br inside a Text is a line feed run");
        check_eq(expr_text(br.props.at("tag").get()), literal("br"), "br keeps its tag");
    } else {
        check(false, "a<br/>b is three runs");
    }

    const auto alone = parse(component("", "<br />", "props: P"));
    const auto alone_result = lower(*alone);
    check(alone_result.has_value(), "a lone br lowers: " + describe_errors(*alone, alone_result));
    if (alone_result) {
        check_eq(std::string(node_kind(alone_result->component.root)), "Text",
                 "a lone br is a Text");
        check_eq(expr_text(alone_result->component.root.text.get()), literal(""),
                 "a lone br is empty");
    }

    const auto children = parse(component("", "<br>x</br>", "props: P"));
    check(!lower(*children).has_value(), "br takes no children");
    const auto rule_children = parse(component("", "<hr>x</hr>", "props: P"));
    check(!lower(*rule_children).has_value(), "hr takes no children");

    const auto rule = parse(component("", "<hr />", "props: P"));
    const auto rule_result = lower(*rule);
    check(rule_result.has_value(), "hr lowers: " + describe_errors(*rule, rule_result));
    if (rule_result) {
        check_eq(std::string(node_kind(rule_result->component.root)), "View", "hr is a View");
        check_eq(expr_text(rule_result->component.root.props.at("tag").get()), literal("hr"),
                 "hr keeps its tag");
    }
}

void test_pre_quote_blockquote() {
    const auto pre = parse(component("", "<pre>a  b</pre>", "props: P"));
    const auto pre_result = lower(*pre);
    check(pre_result.has_value(), "pre lowers: " + describe_errors(*pre, pre_result));
    if (pre_result) {
        check_eq(style_of(pre_result->component.root, "white-space"), literal("pre"),
                 "pre preserves white space");
        check_eq(style_of(pre_result->component.root, "font-family"), literal("monospace"),
                 "pre is monospace");
    }
    const auto quote = parse(component("", "<q>hi</q>", "props: P"));
    const auto quote_result = lower(*quote);
    check(quote_result.has_value(), "q lowers: " + describe_errors(*quote, quote_result));
    if (quote_result) {
        check_eq(expr_text(quote_result->component.root.text.get()), literal("\u201Chi\u201D"),
                 "q wraps its content in curly quotes");
    }
    const auto in_span = parse(component("", "<Text>say <q>hi</q></Text>", "props: P"));
    const auto in_span_result = lower(*in_span);
    check(in_span_result.has_value(),
          "q inside a Text lowers: " + describe_errors(*in_span, in_span_result));
    const auto block = parse(component("", "<blockquote>quoted</blockquote>", "props: P"));
    const auto block_result = lower(*block);
    check(block_result.has_value(), "blockquote lowers: " + describe_errors(*block, block_result));
    if (block_result) {
        check_eq(std::string(node_kind(block_result->component.root)), "View",
                 "blockquote is a View");
    }
}

// `ol start`/`reversed` and `li value` become aria-start/aria-reversed/aria-value (issue #912).
void test_list_numbering_attributes() {
    const auto fixture = parse(component(
        "", "<ol start={-2} reversed><li value={7}>a</li><li value=\"9\">b</li></ol>", "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "list numbering lowers: " + describe_errors(*fixture, result));
    if (result) {
        const ir::Node& list = result->component.root;
        check_eq(expr_text(list.props.at("aria-start").get()), literal("-2"), "start");
        check_eq(expr_text(list.props.at("aria-reversed").get()), literal("true"), "reversed flag");
        check(list.children.size() == 2 &&
                  expr_text(list.children[0].props.at("aria-value").get()) == literal("7") &&
                  expr_text(list.children[1].props.at("aria-value").get()) == literal("9"),
              "item values");
    }
    const auto off = parse(component("", "<ol reversed={false} start=\"3\"/>", "props: P"));
    const auto off_result = lower(*off);
    check(off_result.has_value() &&
              expr_text(off_result->component.root.props.at("aria-reversed").get()) ==
                  literal("false"),
          "reversed={false}");
    const auto dynamic =
        parse(component("  const [n, setN] = useState<int>(1);\n", "<ol start={n}/>", "props: P"));
    check(!lower(*dynamic).has_value(), "a dynamic start is rejected");
}

int main() {
    test_list_numbering_attributes();
    test_group_a_spans();
    test_explicit_decoration_wins();
    test_break_and_rule();
    test_pre_quote_blockquote();
    test_link_span();
    test_emphasis_defaults();
    test_explicit_style_wins();
    test_intrinsic_text_span();
    test_rejections();
    test_alias_outside_span();
    test_unmapped_tag_diagnostic();
    test_alias_tag_conformance();
    return finish("lowering_inline_span");
}
