// SPDX-License-Identifier: MIT

// Slice 7 positive coverage: intrinsic elements, attributes, text runs, style, events, refs, keys,
// fragments and the whole-component assembly on top of the slice 6 hook data.

#include "lowering_nodes_test_util.hpp"

namespace {

using namespace lowering_nodes_test;

void test_static_view_tree() {
    const auto fixture = parse(component(
        "  const [count, setCount] = useState<int>(0);\n",
        "<View className=\"box\" id=\"root\" role=\"generic\" aria-label=\"box\"\n"
        "      style={{ width: 10, opacity: 0.5, color: \"red\", marginTop: props.gap }}>\n"
        "  <Text>Count: {count}</Text>\n"
        "  <Image src=\"icons/a.png\" alt=\"A\" />\n"
        "</View>",
        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "the static tree lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check_eq(result->component.id, "comp::A", "the component id");
    check_eq(result->component.component_id, "", "componentId is left for slice 10");
    check_eq(result->component.name, "A", "the component name");
    check_eq(result->component.source.file, "ui/Counter.tsx", "the component span file");
    check(result->component.source.line == 1, "the component span line");
    check_eq(std::string(node_kind(root)), "View", "the root kind");
    check_eq(root.id, "node::1", "the root id is node::1");
    check_eq(root.ui_id, "", "uiId is left for slice 10");
    check_eq(expr_text(root.props.at("className").get()),
             "{\"kind\":\"literal\",\"value\":\"box\"}", "className is a prop");
    check_eq(expr_text(root.props.at("id").get()), "{\"kind\":\"literal\",\"value\":\"root\"}",
             "id lowers to props.id");
    check_eq(expr_text(root.props.at("role").get()), "{\"kind\":\"literal\",\"value\":\"generic\"}",
             "role lowers to props.role");
    check_eq(expr_text(root.props.at("aria-label").get()),
             "{\"kind\":\"literal\",\"value\":\"box\"}", "aria-* keeps its hyphens");
    check_eq(expr_text(root.style.at("width").get()), "{\"kind\":\"literal\",\"value\":\"10px\"}",
             "a length number becomes px");
    check_eq(expr_text(root.style.at("opacity").get()), "{\"kind\":\"literal\",\"value\":0.5}",
             "a unitless number stays a number");
    check_eq(expr_text(root.style.at("color").get()), "{\"kind\":\"literal\",\"value\":\"red\"}",
             "a color string stays a string");
    check_eq(expr_text(root.style.at("margin-top").get()),
             "{\"kind\":\"ref\",\"id\":\"prop::gap\"}",
             "a camelCase style key becomes kebab-case and a dynamic value is an expression");
    check(root.children.size() == 2, "two children");
    if (root.children.size() != 2) {
        return;
    }
    check_eq(std::string(node_kind(root.children[0])), "Text", "the first child is Text");
    check_eq(root.children[0].id, "node::2", "pre-order id of the first child");
    check_eq(expr_text(root.children[0].text.get()),
             "{\"kind\":\"template\",\"parts\":[\"Count: \",{\"kind\":\"ref\",\"id\":"
             "\"state::count\"}]}",
             "a text run with an expression is a template");
    check_eq(std::string(node_kind(root.children[1])), "Image", "the second child is Image");
    check_eq(root.children[1].id, "node::3", "pre-order id of the second child");
    check_eq(expr_text(root.children[1].props.at("src").get()),
             "{\"kind\":\"literal\",\"value\":\"icons/a.png\"}", "Image src");
    check_eq(expr_text(root.children[1].props.at("alt").get()),
             "{\"kind\":\"literal\",\"value\":\"A\"}", "Image alt");
    check(result->component.state.size() == 1, "the slice 6 state is carried into the component");
}

void test_text_runs() {
    const auto fixture = parse(component("  const [count, setCount] = useState<int>(0);\n",
                                         "<View>a{count}b{true}c{null}d{/* comment */}e</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "the text run lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 1, "all visible parts are one run, hence one synthetic Text");
    if (root.children.size() != 1) {
        return;
    }
    check_eq(std::string(node_kind(root.children[0])), "Text", "a View run is a synthetic Text");
    check_eq(expr_text(root.children[0].text.get()),
             "{\"kind\":\"template\",\"parts\":[\"a\",{\"kind\":\"ref\",\"id\":"
             "\"state::count\"},\"bcde\"]}",
             "booleans, null and comments render nothing and do not break the run");
}

void test_text_element_forms() {
    const auto empty = parse(component("", "<Text />"));
    const auto empty_result = lower(*empty);
    check(empty_result.has_value(), "an empty Text lowers");
    if (empty_result) {
        check_eq(expr_text(empty_result->component.root.text.get()),
                 "{\"kind\":\"literal\",\"value\":\"\"}", "<Text /> has an empty text");
    }

    const auto literal = parse(component("", "<Text>hello</Text>"));
    const auto literal_result = lower(*literal);
    check(literal_result.has_value(), "a literal Text lowers");
    if (literal_result) {
        check_eq(expr_text(literal_result->component.root.text.get()),
                 "{\"kind\":\"literal\",\"value\":\"hello\"}", "text only is a literal string");
        check(literal_result->component.root.children.empty(), "Text has no children");
    }

    const auto dynamic = parse(
        component("  const [count, setCount] = useState<int>(0);\n", "<Text>{count}x</Text>"));
    const auto dynamic_result = lower(*dynamic);
    check(dynamic_result.has_value(), "a dynamic Text lowers");
    if (dynamic_result) {
        check_eq(expr_text(dynamic_result->component.root.text.get()),
                 "{\"kind\":\"template\",\"parts\":[{\"kind\":\"ref\",\"id\":"
                 "\"state::count\"},\"x\"]}",
                 "a single expression is a template with one part");
    }
}

void test_button_label_and_children() {
    const auto text_only = parse(component("", "<Button disabled>Go</Button>"));
    const auto text_result = lower(*text_only);
    check(text_result.has_value(), "a text-only Button lowers");
    if (text_result) {
        const ir::Node& button = text_result->component.root;
        check_eq(expr_text(button.props.at("label").get()),
                 "{\"kind\":\"literal\",\"value\":\"Go\"}", "text-only children become the label");
        check_eq(expr_text(button.props.at("disabled").get()),
                 "{\"kind\":\"literal\",\"value\":true}", "the flag form is true");
        check(button.children.empty(), "a text-only Button has no children");
    }

    const auto mixed =
        parse(component("", "<Button><Image src=\"a.png\" alt=\"Close\" />Save</Button>"));
    const auto mixed_result = lower(*mixed);
    check(mixed_result.has_value(), "a mixed Button lowers");
    if (mixed_result) {
        const ir::Node& button = mixed_result->component.root;
        check(button.props.find("label") == button.props.end(), "an element child means no label");
        check(button.children.size() == 2, "the element and the trailing run are two children");
        if (button.children.size() == 2) {
            check_eq(std::string(node_kind(button.children[0])), "Image",
                     "the element child first");
            check_eq(std::string(node_kind(button.children[1])), "Text", "the run second");
            check_eq(expr_text(button.children[1].text.get()),
                     "{\"kind\":\"literal\",\"value\":\"Save\"}", "the synthetic run text");
        }
    }
}

void test_fragment_splicing() {
    const auto fixture = parse(component("", "<View><><Text>a</Text><Text>b</Text></></View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "fragment children splice: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 2, "the fragment children are spliced");
    if (root.children.size() == 2) {
        check_eq(root.children[0].id, "node::2", "first spliced child id");
        check_eq(root.children[1].id, "node::3", "second spliced child id");
    }
}

void test_events_and_handler_numbering() {
    const auto fixture =
        parse(component("  const [count, setCount] = useState<int>(0);\n"
                        "  const [name, setName] = useState(\"x\");\n"
                        "  // Note: bind::on_click\n"
                        "  const click = (): void => { setCount(1); };\n"
                        "  const bump = () => { setCount(count + 1); };\n",
                        "<View>\n"
                        "  <Button onClick={click}>Go</Button>\n"
                        "  <Button onClick={bump}>Bump</Button>\n"
                        "  <Button onClick={() => setCount(count + 1)}>Inline</Button>\n"
                        "  <TextInput value={name} onChange={setName} />\n"
                        "  <TextInput value={name} onChange={(e) => setName(e.value)} />\n"
                        "</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "events lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 5, "five children");
    if (root.children.size() != 5) {
        return;
    }
    check_eq(root.children[0].events.at("onClick"), "bind::on_click",
             "a declared native handler is its binding id");
    check_eq(root.children[1].events.at("onClick"), "handler::1",
             "a declared inline handler is its record");
    check_eq(root.children[2].events.at("onClick"), "handler::2",
             "an inline arrow gets the next handler number");
    check_eq(root.children[3].events.at("onChange"), "handler::3",
             "a setter reference becomes a handler record");
    check_eq(root.children[4].events.at("onChange"), "handler::4",
             "an arrow written in JSX gets the next handler number");
    check(result->component.handlers.size() == 4, "four handler records");
    if (result->component.handlers.size() != 4) {
        return;
    }
    const ir::Handler& setter = result->component.handlers[2];
    check_eq(setter.id, "handler::3", "the setter record id");
    check(setter.params.size() == 1 && setter.params[0].type == "TextChangeEvent",
          "the setter parameter is typed by the event payload");
    check(setter.actions.size() == 1 && setter.actions[0].target == "state::name",
          "the setter action targets the state");
    check_eq(expr_text(setter.actions[0].value.get()), "{\"kind\":\"ref\",\"id\":\"e.value\"}",
             "a setter receives the primary payload member");
    const ir::Handler& arrow = result->component.handlers[3];
    check(arrow.params.size() == 1 && arrow.params[0].type == "TextChangeEvent",
          "the arrow parameter is typed by the event payload");
    check(arrow.actions.size() == 1, "one action");
    if (arrow.actions.size() == 1) {
        check_eq(expr_text(arrow.actions[0].value.get()), "{\"kind\":\"ref\",\"id\":\"e.value\"}",
                 "the arrow body reads the event payload");
    }
}

void test_ref_key_and_automation_id() {
    const auto fixture =
        parse(component("  const r = useRef<Button>(null);\n",
                        "<View>\n"
                        "  <Button key=\"k\" automationId=\"42\" ref={r}>Go</Button>\n"
                        "</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "key, ref and automationId lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 1, "one child");
    if (root.children.size() != 1) {
        return;
    }
    const ir::Node& button = root.children[0];
    check_eq(expr_text(button.key.get()), "{\"kind\":\"literal\",\"value\":\"k\"}", "the key");
    check_eq(button.automation_id, "42", "automationId is a node field");
    check(result->component.refs.size() == 1, "one ref");
    if (result->component.refs.size() == 1) {
        check_eq(result->component.refs[0].id, "ref::r", "the ref id");
        check_eq(result->component.refs[0].target, button.id, "the ref target is the node id");
    }
}

void test_error_boundary_fallback() {
    const auto fixture =
        parse(component("  // Note: bind::on_error\n"
                        "  const handle = (e: ErrorInfo): void => { };\n",
                        "<ErrorBoundary fallback={<Text>oops</Text>} onError={handle}>\n"
                        "  <View><Text>ok</Text></View>\n"
                        "</ErrorBoundary>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "ErrorBoundary lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check_eq(std::string(node_kind(root)), "ErrorBoundary", "the kind");
    check_eq(root.id, "node::1", "the root id");
    check_eq(root.events.at("onError"), "bind::on_error", "onError is an event");
    check(root.fallback != nullptr, "the fallback subtree exists");
    if (root.fallback != nullptr) {
        check_eq(std::string(node_kind(*root.fallback)), "Text", "the fallback node kind");
        check_eq(root.fallback->id, "node::2", "the fallback gets the next pre-order id");
    }
    check(root.children.size() == 1, "exactly one guarded child");
    if (root.children.size() == 1) {
        check_eq(root.children[0].id, "node::3", "the guarded child follows the fallback");
        check(root.children[0].children.size() == 1, "the guarded subtree");
    }
}

void test_leaf_and_scroll_elements() {
    const auto fixture = parse(
        component("", "<View>\n"
                      "  <Scroll axis=\"x\"><View><Text>a</Text></View></Scroll>\n"
                      "  <Image src=\"a.png\" alt=\"\" fit=\"cover\" />\n"
                      "  <Path d=\"M0 0\" viewBox=\"0 0 1 1\" fill=\"none\" strokeWidth={2} />\n"
                      "  <Spacer grow={1} />\n"
                      "</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "leaf and Scroll elements lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 4, "four children");
    if (root.children.size() != 4) {
        return;
    }
    check_eq(std::string(node_kind(root.children[0])), "Scroll", "Scroll");
    check_eq(expr_text(root.children[0].props.at("axis").get()),
             "{\"kind\":\"literal\",\"value\":\"x\"}", "Scroll axis");
    check_eq(std::string(node_kind(root.children[1])), "Image", "Image");
    check_eq(expr_text(root.children[1].props.at("fit").get()),
             "{\"kind\":\"literal\",\"value\":\"cover\"}", "Image fit");
    check_eq(std::string(node_kind(root.children[2])), "Path", "Path");
    check_eq(expr_text(root.children[2].props.at("strokeWidth").get()),
             "{\"kind\":\"literal\",\"value\":2}", "Path strokeWidth");
    check_eq(std::string(node_kind(root.children[3])), "Spacer", "Spacer");
    check_eq(expr_text(root.children[3].props.at("grow").get()),
             "{\"kind\":\"literal\",\"value\":1}", "Spacer grow");
}

void test_spread_object_literal() {
    const auto fixture =
        parse(component("", "<Button {...{ label: \"Save\", disabled: true }} label=\"Later\" />"));
    const auto result = lower(*fixture);
    check(result.has_value(), "object spread lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& button = result->component.root;
    check_eq(expr_text(button.props.at("label").get()),
             "{\"kind\":\"literal\",\"value\":\"Later\"}", "a later attribute wins over a spread");
    check_eq(expr_text(button.props.at("disabled").get()), "{\"kind\":\"literal\",\"value\":true}",
             "the spread prop is present");
}

void test_style_property_kinds() {
    const auto fixture = parse(component(
        "  const [w, setW] = useState<int>(0);\n",
        "<View style={{ width: w, lineHeight: 1.5, fontFamily: \"monospace\", flexGrow: 2, "
        "backgroundColor: \"#fff\" }} />"));
    const auto result = lower(*fixture);
    check(result.has_value(), "style kinds lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check_eq(expr_text(root.style.at("width").get()), "{\"kind\":\"ref\",\"id\":\"state::w\"}",
             "a dynamic length value is an expression");
    check_eq(expr_text(root.style.at("line-height").get()), "{\"kind\":\"literal\",\"value\":1.5}",
             "line-height keeps a unitless number");
    check_eq(expr_text(root.style.at("font-family").get()),
             "{\"kind\":\"literal\",\"value\":\"monospace\"}", "font-family is a string");
    check_eq(expr_text(root.style.at("flex-grow").get()), "{\"kind\":\"literal\",\"value\":2}",
             "flex-grow is unitless");
    check_eq(expr_text(root.style.at("background-color").get()),
             "{\"kind\":\"literal\",\"value\":\"#fff\"}", "backgroundColor becomes kebab-case");
}

} // namespace

int main() {
    test_static_view_tree();
    test_text_runs();
    test_text_element_forms();
    test_button_label_and_children();
    test_fragment_splicing();
    test_events_and_handler_numbering();
    test_ref_key_and_automation_id();
    test_error_boundary_fallback();
    test_leaf_and_scroll_elements();
    test_spread_object_literal();
    test_style_property_kinds();
    return finish("lowering nodes tests");
}
