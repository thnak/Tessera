// SPDX-License-Identifier: MIT

// `a href` and `label htmlFor` (issue #542, ADR-059 group B, html-aliases.md section 3.5): the
// rewrite to `aria-href` / `aria-for`, the Tab stop a link gets, the `target`/`rel` values a port
// writes everywhere, and the refusals (dynamic or oversize values, an inline span).

#include "lowering_nodes_test_util.hpp"

#include <string>

namespace {

using namespace lowering_nodes_test;

std::string literal_of(const char* text) {
    return std::string("{\"kind\":\"literal\",\"value\":\"") + text + "\"}";
}

void test_link_node() {
    const auto fixture =
        parse(component("", "<a href=\"/settings\" id=\"s\">Settings</a>", "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a link node lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& link = result->component.root;
    check_eq(std::string(node_kind(link)), "Text", "an anchor is a Text");
    check_eq(expr_text(link.props.at("aria-href").get()), literal_of("/settings"), "href");
    check_eq(expr_text(link.props.at("role").get()), literal_of("link"), "role link");
    check(link.props.count("tabIndex") == 1, "a link is a Tab stop");
    check(link.props.count("href") == 0, "the attribute is renamed, not kept");
}

void test_link_forms() {
    const auto external = parse(component("",
                                          "<a href={\"https://example.com/docs\"} "
                                          "target=\"_blank\" rel=\"noopener noreferrer\">d</a>",
                                          "props: P"));
    const auto result = lower(*external);
    check(result.has_value(), "an external link lowers: " + describe_errors(*external, result));
    if (result) {
        check_eq(expr_text(result->component.root.props.at("aria-href").get()),
                 literal_of("https://example.com/docs"), "an expression string literal works");
        check(result->component.root.props.count("target") == 0 &&
                  result->component.root.props.count("rel") == 0,
              "target and rel are dropped");
    }

    const auto explicit_tab =
        parse(component("", "<a href=\"/x\" tabIndex={-1}>x</a>", "props: P"));
    const auto kept = lower(*explicit_tab);
    check(kept.has_value() && expr_text(kept->component.root.props.at("tabIndex").get()) ==
                                  "{\"kind\":\"literal\",\"value\":-1}",
          "an explicit tabIndex wins over the injected one");
}

void test_label_for() {
    const auto fixture = parse(
        component("", "<View><label htmlFor=\"agree\">I agree</label><input id=\"agree\"/></View>",
                  "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a label lowers: " + describe_errors(*fixture, result));
    if (!result || result->component.root.children.size() != 2) {
        return;
    }
    const ir::Node& label = result->component.root.children[0];
    check_eq(expr_text(label.props.at("aria-for").get()), literal_of("agree"), "htmlFor");
    check(label.props.count("tabIndex") == 0, "a label is not a Tab stop");
}

void test_dynamic_label_for() {
    const auto fixture = parse(component(
        "", "<View><label htmlFor={props.id}>L</label><input id={props.id}/></View>", "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a dynamic htmlFor lowers: " + describe_errors(*fixture, result));
    if (result && !result->component.root.children.empty()) {
        check(result->component.root.children[0].props.count("aria-for") == 1,
              "the dynamic id rides the aria-for pair");
    }
}

void test_refusals() {
    const auto dynamic = parse(component("  const [u, setU] = useState<string>(\"/a\");\n",
                                         "<a href={u}>x</a>", "props: P"));
    check(!lower(*dynamic).has_value(), "a dynamic href is refused (the pair is emitted once)");

    const auto empty = parse(component("", "<a href=\"\">x</a>", "props: P"));
    check(!lower(*empty).has_value(), "an empty href is refused");

    const auto long_href = parse(component(
        "", "<a href=\"https://example.com/" + std::string(120, 'a') + "\">x</a>", "props: P"));
    check(!lower(*long_href).has_value(), "an href past 128 bytes is refused, not truncated");

    const auto bad_target =
        parse(component("", "<a href=\"/x\" target=\"popup\">x</a>", "props: P"));
    check_eq(first_code(lower(*bad_target)), "TESSERA_JSX_SUBSET_HTML_ATTR",
             "an unknown target is an HTML_ATTR error");

    const auto download = parse(component("", "<a href=\"/x\" download>x</a>", "props: P"));
    check(!lower(*download).has_value(), "download stays unsupported");

    const auto span = parse(component("", "<Text>see <a href=\"/x\">this</a></Text>", "props: P"));
    check_eq(first_code(lower(*span)), "TESSERA_JSX_SPAN_ATTRIBUTE",
             "an href on an inline span is refused with the span code");

    const auto span_label =
        parse(component("", "<Text>see <label htmlFor=\"x\">this</label></Text>", "props: P"));
    check_eq(first_code(lower(*span_label)), "TESSERA_JSX_SPAN_ATTRIBUTE",
             "htmlFor on an inline span is refused too");

    const auto capital = parse(component("", "<Text href=\"/x\">x</Text>", "props: P"));
    check(!lower(*capital).has_value(), "the rewrite is for the lowercase tag only (rule 4)");
}

} // namespace

int main() {
    test_link_node();
    test_link_forms();
    test_label_for();
    test_dynamic_label_for();
    test_refusals();
    return finish("lowering_links_labels");
}
