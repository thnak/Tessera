// SPDX-License-Identifier: MIT

// Slice 8 virtual-collection coverage, split from `lowering_control_tests.cpp` at the 500-line
// limit: a `VirtualList`/`VirtualGrid` item template, its geometry defaults and its collection
// events. The plain conditional/list form is in the sibling file.

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

void test_virtual_collection_events() {
    const auto fixture =
        parse(component("  const [count, setCount] = useState<int>(0);\n",
                        "<VirtualList itemSize={32} onScroll={(e: ScrollEvent) => setCount(1)} "
                        "onEndReached={() => setCount(2)}>{props.rows.map((row) => "
                        "<Text key={row.id}>{row.label}</Text>)}</VirtualList>",
                        "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "the VirtualList collection events lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const ir::Node& list = result->component.root;
    check(list.events.size() == 2, "both collection-level events are recorded");
    check(list.events.count("onScroll") == 1, "onScroll is recorded");
    check(list.events.count("onEndReached") == 1, "onEndReached is recorded");
    const auto handler_for = [&](std::string_view event) -> const ir::Handler* {
        const auto found = list.events.find(std::string(event));
        if (found == list.events.end()) {
            return nullptr;
        }
        for (const ir::Handler& handler : result->component.handlers) {
            if (handler.id == found->second) {
                return &handler;
            }
        }
        return nullptr;
    };
    const ir::Handler* scroll = handler_for("onScroll");
    check(scroll != nullptr, "onScroll names a handler record");
    if (scroll != nullptr) {
        check(scroll->params.size() == 1, "onScroll takes one parameter");
        if (scroll->params.size() == 1) {
            check_eq(scroll->params[0].type, "ScrollEvent",
                     "onScroll carries the scene ScrollEvent payload record");
        }
    }
    const ir::Handler* end = handler_for("onEndReached");
    check(end != nullptr, "onEndReached names a handler record");
    if (end != nullptr) {
        check(end->params.empty(), "onEndReached carries no payload");
        check(end->actions.size() == 1, "the inline onEndReached body lowers");
    }
}

} // namespace

int main() {
    test_virtual_list_item_template();
    test_virtual_grid_item_template();
    test_virtual_default_estimate();
    test_virtual_collection_events();
    test_conditional_inside_scroll();
    return finish("lowering virtual control tests");
}
