// SPDX-License-Identifier: MIT

// Slice 8 positive coverage: conditional children (`&&`, `?:`, null/false branches), `.map` list
// loops with keys and loop variables, and their composition with the slice 7 attributes, events
// and text runs.

#include "lowering_nodes_test_util.hpp"

namespace {

using namespace lowering_nodes_test;

std::string condition_text(const ir::Node& node) {
    return node.condition ? expr_text(node.condition.get()) : std::string("<none>");
}

std::string loop_text(const ir::Node& node) {
    if (!node.loop) {
        return "<none>";
    }
    return "item=" + node.loop->item + " index=" + node.loop->index +
           " source=" + expr_text(node.loop->source.get());
}

const ir::Node* only_child(const ir::Node& parent) {
    check(parent.children.size() == 1, "one child");
    return parent.children.size() == 1 ? &parent.children[0] : nullptr;
}

void test_guarded_conditional() {
    const auto fixture = parse(component("  const [flag, setFlag] = useState(true);\n",
                                         "<View>{flag && <Text>a</Text>}</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "the guarded conditional lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node* child = only_child(result->component.root);
    if (child == nullptr) {
        return;
    }
    check_eq(std::string(node_kind(*child)), "Text", "the branch is a Text node");
    check_eq(child->id, "node::2", "the branch follows the root id");
    check_eq(condition_text(*child), "{\"kind\":\"ref\",\"id\":\"state::flag\"}",
             "`&&` sets the condition expression");
    check_eq(expr_text(child->text.get()), "{\"kind\":\"literal\",\"value\":\"a\"}",
             "the branch text is lowered normally");
}

void test_ternary_both_branches() {
    const auto fixture = parse(component("  const [flag, setFlag] = useState(true);\n",
                                         "<View>{flag ? <Text>a</Text> : <Text>b</Text>}</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "the ternary lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 2, "a two-element ternary is two sibling nodes");
    if (root.children.size() != 2) {
        return;
    }
    check_eq(root.children[0].id, "node::2", "the then branch gets the first id");
    check_eq(root.children[1].id, "node::3", "the else branch gets the next id");
    check_eq(condition_text(root.children[0]), "{\"kind\":\"ref\",\"id\":\"state::flag\"}",
             "the then condition is the test");
    check_eq(condition_text(root.children[1]),
             "{\"kind\":\"unary\",\"op\":\"!\",\"value\":{\"kind\":\"ref\",\"id\":"
             "\"state::flag\"}}",
             "the else condition is the negated test");
}

void test_ternary_nothing_branches() {
    const auto then_null = parse(component("  const [flag, setFlag] = useState(true);\n",
                                           "<View>{flag ? <Text>a</Text> : null}</View>"));
    const auto then_null_result = lower(*then_null);
    check(then_null_result.has_value(), "a null else branch lowers");
    if (then_null_result) {
        const ir::Node* child = only_child(then_null_result->component.root);
        if (child != nullptr) {
            check_eq(condition_text(*child), "{\"kind\":\"ref\",\"id\":\"state::flag\"}",
                     "the null branch adds no node");
        }
    }

    const auto else_null = parse(component("  const [flag, setFlag] = useState(true);\n",
                                           "<View>{flag ? null : <Text>b</Text>}</View>"));
    const auto else_null_result = lower(*else_null);
    check(else_null_result.has_value(), "a null then branch lowers");
    if (else_null_result) {
        const ir::Node* child = only_child(else_null_result->component.root);
        if (child != nullptr) {
            check_eq(condition_text(*child),
                     "{\"kind\":\"unary\",\"op\":\"!\",\"value\":{\"kind\":\"ref\",\"id\":"
                     "\"state::flag\"}}",
                     "a null then branch keeps the else node with the negated condition");
        }
    }

    const auto false_branch = parse(component("  const [flag, setFlag] = useState(true);\n",
                                              "<View>{flag ? <Text>a</Text> : false}</View>"));
    const auto false_result = lower(*false_branch);
    check(false_result.has_value(), "a false branch renders nothing");
    if (false_result) {
        check(false_result->component.root.children.size() == 1, "one node for the then branch");
    }
}

void test_loop_shape() {
    const auto fixture = parse(component(
        "",
        "<View>{props.rows.map((row) => <View key={row.id}><Text>{row.label}</Text></View>)}"
        "</View>",
        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "the loop lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node* template_node = only_child(result->component.root);
    if (template_node == nullptr) {
        return;
    }
    check_eq(std::string(node_kind(*template_node)), "View", "the template is the loop node");
    check_eq(template_node->id, "node::2", "the template gets the next pre-order id");
    check_eq(loop_text(*template_node),
             "item=row index= source={\"kind\":\"ref\",\"id\":"
             "\"prop::rows\"}",
             "the loop binds the item and the source ref");
    check_eq(expr_text(template_node->key.get()), "{\"kind\":\"ref\",\"id\":\"row.id\"}",
             "the key attribute becomes the loop key");
    check(template_node->children.size() == 1, "the loop body is the template subtree");
    if (template_node->children.size() == 1) {
        check_eq(template_node->children[0].id, "node::3", "the loop body child id");
        check_eq(expr_text(template_node->children[0].text.get()),
                 "{\"kind\":\"template\",\"parts\":[{\"kind\":\"ref\",\"id\":\"row.label\"}]}",
                 "loop item fields are refs");
    }
}

void test_loop_index() {
    const auto fixture = parse(
        component("",
                  "<View>{props.rows.map((row, index) => <View key={index}><Text>{row.label}</Text>"
                  "</View>)}</View>",
                  "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "the indexed loop lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node* template_node = only_child(result->component.root);
    if (template_node == nullptr) {
        return;
    }
    check_eq(loop_text(*template_node),
             "item=row index=index source={\"kind\":\"ref\",\"id\":"
             "\"prop::rows\"}",
             "the second callback parameter is the loop index");
    check_eq(expr_text(template_node->key.get()), "{\"kind\":\"ref\",\"id\":\"index\"}",
             "the index is usable in the key");
}

void test_nested_loops_depth_two() {
    const auto fixture = parse(
        component("",
                  "<View>{props.rows.map((row) => <View key={row.id}>{row.children.map((child) => "
                  "<Text key={child.id}>{child.label}</Text>)}</View>)}</View>",
                  "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a depth-2 loop lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node* outer = only_child(result->component.root);
    check(outer != nullptr && outer->loop.has_value(), "the outer node carries the loop");
    if (outer == nullptr || !outer->loop.has_value()) {
        return;
    }
    check_eq(outer->loop->item, "row", "outer loop item");
    const ir::Node* inner = outer->children.size() == 1 ? &outer->children[0] : nullptr;
    check(inner != nullptr, "the inner loop is the outer template's child");
    if (inner == nullptr) {
        return;
    }
    check_eq(std::string(node_kind(*inner)), "Text", "the inner loop node is the template");
    check_eq(loop_text(*inner),
             "item=child index= source={\"kind\":\"ref\",\"id\":\"row.children\"}",
             "the inner loop source reads the outer item");
    check_eq(expr_text(inner->key.get()), "{\"kind\":\"ref\",\"id\":\"child.id\"}",
             "the inner key reads the inner item");
    check_eq(inner->id, "node::3", "nested ids stay in source pre-order");
}

void test_ids_across_branches() {
    const auto fixture = parse(component(
        "  const [flag, setFlag] = useState(true);\n",
        "<View>{flag ? <View><Text>a</Text></View> : <View><Text>b</Text></View>}</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a conditional with subtrees lowers");
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 2, "two branch roots");
    if (root.children.size() != 2 || root.children[0].children.size() != 1 ||
        root.children[1].children.size() != 1) {
        check(false, "both branches keep their subtrees");
        return;
    }
    check_eq(root.children[0].id, "node::2", "then branch root id");
    check_eq(root.children[0].children[0].id, "node::3", "then branch subtree id");
    check_eq(root.children[1].id, "node::4", "else branch root id");
    check_eq(root.children[1].children[0].id, "node::5", "else branch subtree id");
}

void test_handlers_in_branches_are_not_reused() {
    const auto fixture =
        parse(component("  const [count, setCount] = useState<int>(0);\n"
                        "  const [flag, setFlag] = useState(true);\n",
                        "<View>{flag ? <Button onClick={() => setCount(1)}>A</Button> : "
                        "<Button onClick={() => setCount(2)}>B</Button>}</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "handlers in both branches lower");
    if (!result) {
        return;
    }
    const ir::Node& root = result->component.root;
    check(root.children.size() == 2, "two branch nodes");
    if (root.children.size() != 2) {
        return;
    }
    check_eq(root.children[0].events.at("onClick"), "handler::1", "the then branch handler");
    check_eq(root.children[1].events.at("onClick"), "handler::2", "the else branch handler");
    check(result->component.handlers.size() == 2, "two handler records");
    if (result->component.handlers.size() == 2) {
        check_eq(expr_text(result->component.handlers[0].actions[0].value.get()),
                 "{\"kind\":\"literal\",\"value\":1}", "the then branch action");
        check_eq(expr_text(result->component.handlers[1].actions[0].value.get()),
                 "{\"kind\":\"literal\",\"value\":2}", "the else branch action");
    }
    check_eq(expr_text(root.children[0].props.at("label").get()),
             "{\"kind\":\"literal\",\"value\":\"A\"}", "attributes in a branch lower normally");
}

void test_conditional_inside_loop() {
    const auto fixture =
        parse(component("",
                        "<View>{props.rows.map((row) => <View key={row.id}>{row.open ? "
                        "<Text>{row.label}</Text> : null}</View>)}</View>",
                        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a conditional inside a loop body lowers");
    if (!result) {
        return;
    }
    const ir::Node* outer = only_child(result->component.root);
    if (outer == nullptr || outer->children.size() != 1) {
        check(false, "the loop template has a conditional child");
        return;
    }
    const ir::Node& guarded = outer->children[0];
    check_eq(condition_text(guarded), "{\"kind\":\"ref\",\"id\":\"row.open\"}",
             "the condition reads the loop item");
    check_eq(expr_text(guarded.text.get()),
             "{\"kind\":\"template\",\"parts\":[{\"kind\":\"ref\",\"id\":\"row.label\"}]}",
             "the guarded text reads the loop item");
}

void test_loop_inside_conditional() {
    const auto fixture = parse(component(
        "  const [flag, setFlag] = useState(true);\n",
        "<View>{flag ? <View>{props.rows.map((row) => <Text key={row.id}>{row.label}</Text>)}"
        "</View> : null}</View>",
        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a loop inside a conditional branch lowers");
    if (!result) {
        return;
    }
    const ir::Node* branch = only_child(result->component.root);
    if (branch == nullptr) {
        return;
    }
    check_eq(condition_text(*branch), "{\"kind\":\"ref\",\"id\":\"state::flag\"}",
             "the branch carries the condition");
    const ir::Node* loop = branch->children.size() == 1 ? &branch->children[0] : nullptr;
    check(loop != nullptr, "the loop is inside the branch");
    if (loop != nullptr) {
        check_eq(loop_text(*loop),
                 "item=row index= source={\"kind\":\"ref\",\"id\":\"prop::rows\"}",
                 "the nested loop is lowered");
    }
}

void test_text_conditional_stays_a_run() {
    const auto fixture = parse(component("  const [flag, setFlag] = useState(true);\n",
                                         "<View>{flag ? \"a\" : \"b\"}</View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a text conditional lowers as a run expression");
    if (!result) {
        return;
    }
    const ir::Node* child = only_child(result->component.root);
    if (child == nullptr) {
        return;
    }
    check_eq(std::string(node_kind(*child)), "Text",
             "a text-only conditional stays a synthetic Text run");
    check_eq(expr_text(child->text.get()),
             "{\"kind\":\"template\",\"parts\":[{\"kind\":\"conditional\",\"test\":"
             "{\"kind\":\"ref\",\"id\":\"state::flag\"},\"then\":{\"kind\":\"literal\","
             "\"value\":\"a\"},\"else\":{\"kind\":\"literal\",\"value\":\"b\"}}]}",
             "the conditional expression is lowered by slice 5 as one template part");
}

void test_virtual_list_item_template() {
    const auto fixture =
        parse(component("",
                        "<VirtualList itemSize={32} gap={4}>{props.rows.map((row, i) => "
                        "<View key={row.id}><Text>{row.label}</Text></View>)}</VirtualList>",
                        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "the VirtualList item template lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& list = result->component.root;
    check_eq(std::string(node_kind(list)), "VirtualList", "the collection keeps its kind");
    check(list.children.empty(), "the collection has no children of its own");
    check(list.loop == std::nullopt, "the collection is not itself looped");
    check_eq(expr_text(list.props.at("itemSize").get()), "{\"kind\":\"literal\",\"value\":32}",
             "itemSize stays a collection prop");
    check_eq(expr_text(list.props.at("gap").get()), "{\"kind\":\"literal\",\"value\":4}",
             "gap stays a collection prop");
    check(list.item_template != nullptr, "the `.map` child becomes the itemTemplate");
    if (list.item_template == nullptr) {
        return;
    }
    const ir::Node& item = *list.item_template;
    check_eq(std::string(node_kind(item)), "View", "the callback element is the template root");
    check_eq(item.id, "node::2", "the template subtree takes the next pre-order ids");
    check_eq(loop_text(item), "item=row index=i source={\"kind\":\"ref\",\"id\":\"prop::rows\"}",
             "the callback parameters and the `.map` receiver are the loop");
    check_eq(expr_text(item.key.get()), "{\"kind\":\"ref\",\"id\":\"row.id\"}",
             "the key is the item expression");
    check(item.children.size() == 1, "the template keeps its subtree");
    if (item.children.size() == 1) {
        check_eq(item.children[0].id, "node::3", "the template subtree id");
        check_eq(expr_text(item.children[0].text.get()),
                 "{\"kind\":\"template\",\"parts\":[{\"kind\":\"ref\",\"id\":\"row.label\"}]}",
                 "loop item fields are refs inside the template");
    }
}

void test_virtual_grid_item_template() {
    const auto fixture =
        parse(component("",
                        "<VirtualGrid columns={2} rowHeight={40}>{props.cells.map((cell) => "
                        "<Text key={cell}>{cell}</Text>)}</VirtualGrid>",
                        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "the VirtualGrid item template lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& grid = result->component.root;
    check_eq(std::string(node_kind(grid)), "VirtualGrid", "the grid kind");
    check(grid.item_template != nullptr && grid.item_template->loop.has_value(),
          "the grid carries the item template with its loop");
    if (grid.item_template != nullptr) {
        check_eq(grid.item_template->loop->source->id, "prop::cells", "the grid data source");
        check_eq(expr_text(grid.item_template->key.get()), "{\"kind\":\"ref\",\"id\":\"cell\"}",
                 "the grid key is the item expression");
    }
    check_eq(expr_text(grid.props.at("rowHeight").get()), "{\"kind\":\"literal\",\"value\":40}",
             "rowHeight stays a grid prop");
}

void test_virtual_default_estimate() {
    const auto fixture = parse(
        component("",
                  "<VirtualList>{props.rows.map((row) => <Text key={row.id}>{row.label}</Text>)}"
                  "</VirtualList>",
                  "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "a collection without a size prop lowers: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& list = result->component.root;
    const auto found = list.props.find("estimateSize");
    check(found != list.props.end(), "the 32 px estimate is recorded as estimateSize");
    if (found != list.props.end()) {
        check_eq(expr_text(found->second.get()), "{\"kind\":\"literal\",\"value\":32}",
                 "the default estimate is the section 4 register value");
    }
    check(list.props.find("itemSize") == list.props.end(),
          "the default estimate does not invent a fixed itemSize");
}

void test_conditional_inside_scroll() {
    const auto fixture = parse(component("  const [flag, setFlag] = useState(true);\n",
                                         "<Scroll>{flag && <View><Text>a</Text></View>}</Scroll>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a conditional child is a node inside Scroll");
    if (!result) {
        return;
    }
    const ir::Node* child = only_child(result->component.root);
    if (child != nullptr) {
        check_eq(std::string(node_kind(*child)), "View", "the guarded node");
        check_eq(condition_text(*child), "{\"kind\":\"ref\",\"id\":\"state::flag\"}",
                 "the guarded node condition");
    }
}

} // namespace

int main() {
    test_guarded_conditional();
    test_ternary_both_branches();
    test_ternary_nothing_branches();
    test_loop_shape();
    test_loop_index();
    test_nested_loops_depth_two();
    test_ids_across_branches();
    test_handlers_in_branches_are_not_reused();
    test_conditional_inside_loop();
    test_loop_inside_conditional();
    test_text_conditional_stays_a_run();
    test_conditional_inside_scroll();
    test_virtual_list_item_template();
    test_virtual_grid_item_template();
    test_virtual_default_estimate();
    return finish("lowering control tests");
}
