// SPDX-License-Identifier: MIT

// Inline spans (issue #229, elements-basic.md section 2.1): a Text may contain Text children that
// become attributed runs, the emphasis aliases map to spans with injected default styles, and the
// V1 limits are reported as first errors.

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
    {
        // Alias resolution outside a span is still slice 13; the diagnostic is unchanged.
        const auto fixture = parse(component("", "<b>x</b>", "props: P"));
        const auto result = lower(*fixture);
        check(!result && first_code(result) == "TESSERA_IR_UNSUPPORTED",
              "an alias outside a Text span is still refused");
    }
}

} // namespace

int main() {
    test_link_span();
    test_emphasis_defaults();
    test_explicit_style_wins();
    test_intrinsic_text_span();
    test_rejections();
    return finish("lowering_inline_span");
}
