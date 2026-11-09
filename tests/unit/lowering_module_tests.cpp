// SPDX-License-Identifier: MIT

// Slice 9 positive coverage: module-level lowering of one or more files, component references with
// props/callback props/children, components inside conditionals and loops, recursion, and
// cross-file imports resolved by `lower_project`.

#include "lowering_module_test_util.hpp"

namespace {

using namespace lowering_module_test;

constexpr std::string_view kCard = "export function Card(props: P) {\n"
                                   "  return <View><Text>{props.title}</Text></View>;\n"
                                   "}\n";

void test_single_module_components_and_order() {
    const auto fixture =
        parse_file("ui/App.tsx", std::string(kCard) + "function App() {\n"
                                                      "  return <Card title=\"x\" count={1} />;\n"
                                                      "}\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "factory");
    check(result.has_value(), "the module lowers");
    if (!result) {
        return;
    }
    check_eq(result->name, "factory", "the module name is passed through");
    check(result->components.size() == 2, "two components");
    if (result->components.size() != 2) {
        return;
    }
    check_eq(result->components[0].name, "App", "components are sorted by name in one file");
    check_eq(result->components[1].name, "Card", "components are sorted by name in one file");
    check_eq(result->components[0].id, "comp::App", "the component id");
    const ir::Node& root = result->components[0].root;
    check_eq(std::string(ir::node_kind_name(root.kind)), "Component", "the reference node kind");
    check_eq(root.component, "comp::Card", "the referenced component id");
    check_eq(root.id, "node::1", "the reference node id");
    check_eq(expr_text(root.props.at("title").get()), "{\"kind\":\"literal\",\"value\":\"x\"}",
             "a literal prop");
    check_eq(expr_text(root.props.at("count").get()), "{\"kind\":\"literal\",\"value\":1}",
             "a numeric prop");
    const ir::Node& card_root = result->components[1].root;
    check_eq(std::string(ir::node_kind_name(card_root.kind)), "View",
             "the declared component root");
}

void test_children_forwarding() {
    const auto fixture = parse_file("ui/App.tsx", "function Card(props: P) { return <View />; }\n"
                                                  "function App() {\n"
                                                  "  return <Card><Text>hi</Text>tail</Card>;\n"
                                                  "}\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(result.has_value(), "children forward to the reference");
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr, "App is present");
    if (app == nullptr) {
        return;
    }
    const ir::Node& root = app->root;
    check_eq(root.component, "comp::Card", "the reference");
    check(root.children.size() == 2, "the element child and the trailing text run");
    if (root.children.size() != 2) {
        return;
    }
    check_eq(std::string(ir::node_kind_name(root.children[0].kind)), "Text", "the element child");
    check_eq(expr_text(root.children[0].text.get()), "{\"kind\":\"literal\",\"value\":\"hi\"}",
             "the element child text");
    check_eq(std::string(ir::node_kind_name(root.children[1].kind)), "Text",
             "text runs become synthetic Text children of the caller tree");
    check_eq(expr_text(root.children[1].text.get()), "{\"kind\":\"literal\",\"value\":\"tail\"}",
             "the synthetic text");
}

void test_component_inside_conditional_and_loop() {
    const auto fixture = parse_file(
        "ui/App.tsx", "function Row(props: P) { return <Text>{props.label}</Text>; }\n"
                      "function App(props: P) {\n"
                      "  const [flag, setFlag] = useState(true);\n"
                      "  return <View>\n"
                      "    {flag && <Row label=\"a\" />}\n"
                      "    {props.rows.map((row) => <Row key={row.id} label={row.label} />)}\n"
                      "  </View>;\n"
                      "}\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(result.has_value(), "component references compose with slices 7/8");
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    if (app == nullptr) {
        check(false, "App is present");
        return;
    }
    const ir::Node& root = app->root;
    check(root.children.size() == 2, "two control children");
    if (root.children.size() != 2) {
        return;
    }
    const ir::Node& guarded = root.children[0];
    check_eq(std::string(ir::node_kind_name(guarded.kind)), "Component", "the guarded reference");
    check_eq(guarded.component, "comp::Row", "the guarded reference target");
    check_eq(expr_text(guarded.condition.get()), "{\"kind\":\"ref\",\"id\":\"state::flag\"}",
             "the guard condition");
    const ir::Node& looped = root.children[1];
    check_eq(std::string(ir::node_kind_name(looped.kind)), "Component", "the loop reference");
    check(looped.loop.has_value(), "the reference carries the loop");
    if (looped.loop.has_value()) {
        check_eq(looped.loop->item, "row", "the loop item");
        check_eq(expr_text(looped.loop->source.get()), "{\"kind\":\"ref\",\"id\":\"prop::rows\"}",
                 "the loop source");
    }
}

void test_recursive_and_mutually_recursive_components() {
    const auto fixture =
        parse_file("ui/Tree.tsx", "function Node(props: P) {\n"
                                  "  return <View>{props.depth > 0 ? <Node depth={props.depth - "
                                  "1} /> : null}</View>;\n"
                                  "}\n"
                                  "function A(props: P) { return <B />; }\n"
                                  "function B(props: P) { return <A />; }\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(result.has_value(),
          "a recursive component reference lowers (references are not inlined)");
    if (!result) {
        return;
    }
    const ir::Component* node = find_component(*result, "Node");
    const ir::Component* a = find_component(*result, "A");
    const ir::Component* b = find_component(*result, "B");
    check(node != nullptr && a != nullptr && b != nullptr, "all three components are present");
    if (node == nullptr || a == nullptr || b == nullptr) {
        return;
    }
    check(node->root.children.size() == 1, "the recursive reference is a conditional child");
    if (node->root.children.size() == 1) {
        const ir::Node& recursive = node->root.children[0];
        check_eq(recursive.component, "comp::Node", "the recursive reference targets itself");
        check(recursive.condition != nullptr, "the recursive reference is conditional");
    }
    check_eq(a->root.component, "comp::B", "A references B");
    check_eq(b->root.component, "comp::A", "B references A");
}

void test_component_props_events_ref_and_automation_id() {
    const auto fixture = parse_file(
        "ui/App.tsx",
        "function Card(props: P) { return <View />; }\n"
        "function App() {\n"
        "  const [count, setCount] = useState<int>(0);\n"
        "  const r = useRef<Card>(null);\n"
        "  // Note: bind::on_pick\n"
        "  const pick = (): void => { setCount(1); };\n"
        "  return <Card ref={r} automationId=\"7\" className=\"card\" id=\"c\" role=\"button\" "
        "aria-label=\"pick\" {...{ disabled: true }} onPick={pick} onTap={() => setCount(2)} />;\n"
        "}\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(result.has_value(), "component props, events and refs lower");
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    if (app == nullptr) {
        check(false, "App is present");
        return;
    }
    const ir::Node& root = app->root;
    check_eq(root.automation_id, "7", "automationId is a node field");
    check_eq(expr_text(root.props.at("className").get()),
             "{\"kind\":\"literal\",\"value\":\"card\"}", "className is a component prop");
    check_eq(expr_text(root.props.at("id").get()), "{\"kind\":\"literal\",\"value\":\"c\"}",
             "id is a component prop");
    check_eq(expr_text(root.props.at("aria-label").get()),
             "{\"kind\":\"literal\",\"value\":\"pick\"}", "aria-* keeps its hyphens");
    check_eq(expr_text(root.props.at("disabled").get()), "{\"kind\":\"literal\",\"value\":true}",
             "an object-literal spread prop");
    check_eq(root.events.at("onPick"), "bind::on_pick", "a declared handler callback prop");
    check_eq(root.events.at("onTap"), "handler::1", "an inline zero-parameter callback prop");
    check(app->handlers.size() == 1, "one inline handler record");
    check(app->refs.size() == 1, "one ref");
    if (app->refs.size() == 1) {
        check_eq(app->refs[0].target, root.id, "the ref targets the component node");
    }
}

void test_component_as_error_boundary_fallback() {
    const auto fixture = parse_file(
        "ui/App.tsx", "function Card(props: P) { return <View />; }\n"
                      "function App() {\n"
                      "  return <ErrorBoundary fallback={<Card />}><View /></ErrorBoundary>;\n"
                      "}\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(result.has_value(), "a component reference is a valid fallback element");
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    if (app == nullptr) {
        check(false, "App is present");
        return;
    }
    check(app->root.fallback != nullptr, "the fallback exists");
    if (app->root.fallback != nullptr) {
        check_eq(std::string(ir::node_kind_name(app->root.fallback->kind)), "Component",
                 "the fallback is a component reference");
        check_eq(app->root.fallback->component, "comp::Card", "the fallback target");
    }
}

void test_project_imports_and_ordering() {
    auto card = parse_file(
        "ui/Card.tsx", "export type Row = { id: int };\n"
                       "export function Card(props: P) { return <Text>{props.label}</Text>; }\n");
    auto app =
        parse_file("ui/App.tsx", "import { Card } from \"./Card\";\n"
                                 "import { Row } from \"./Card\";\n"
                                 "export function App() { return <Card label=\"hi\" />; }\n");
    auto shell = parse_file("ui/shell/Main.tsx", "import { App } from \"../App\";\n"
                                                 "export function Main() { return <App />; }\n");
    Project project;
    project.add(std::move(shell));
    project.add(std::move(app));
    project.add(std::move(card));
    const auto result = project.lower("factory");
    check(result.has_value(), "a multi-file project lowers");
    if (!result) {
        return;
    }
    check(result->components.size() == 3, "three components");
    if (result->components.size() != 3) {
        return;
    }
    // ir-v1 section 11: source path, then name. "ui/App.tsx" < "ui/Card.tsx" < "ui/shell/Main.tsx".
    check_eq(result->components[0].name, "App", "the first component is App");
    check_eq(result->components[1].name, "Card", "the second component is Card");
    check_eq(result->components[2].name, "Main", "the third component is Main");
    check_eq(result->components[0].root.component, "comp::Card", "the imported reference");
    check_eq(result->components[2].root.component, "comp::App", "the parent-directory import");
}

void test_module_source_preconditions() {
    const auto fixture = parse_file("ui/App.tsx", "function App() { return <View />; }\n");
    lowering::ModuleSource broken = fixture->module_source();
    broken.analysis = nullptr;
    const auto result = lowering::lower_module(broken, "m");
    check(!result.has_value(), "a source without its analysis is rejected");
    check_eq(first_code(result), "TESSERA_IR_MALFORMED", "the code");
}

} // namespace

int main() {
    test_single_module_components_and_order();
    test_children_forwarding();
    test_component_inside_conditional_and_loop();
    test_recursive_and_mutually_recursive_components();
    test_component_props_events_ref_and_automation_id();
    test_component_as_error_boundary_fallback();
    test_project_imports_and_ordering();
    test_module_source_preconditions();
    return finish("lowering module tests");
}
