// SPDX-License-Identifier: MIT

// IR validation of node shape, loops, expressions, types, spans and ordering.

#include "ir_validate_util.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ir_test;

ir::Node leaf(ir::NodeKind kind, const std::string& id, const std::string& ui_id) {
    ir::Node node;
    node.kind = kind;
    node.id = id;
    node.ui_id = ui_id;
    node.source = span(1, 1, 1, 1, 1, 2);
    return node;
}

void test_node_shapes() {
    expect(
        "custom element without an element id",
        [](auto& m) {
            label_node(m).text.reset();
            label_node(m).kind = ir::NodeKind::CustomElement;
        },
        kUnresolved);
    expect("element on a View", [](auto& m) { label_node(m).element = "X"; }, kMalformed);
    expect(
        "custom element with an element id",
        [](auto& m) {
            label_node(m).text.reset();
            label_node(m).kind = ir::NodeKind::CustomElement;
            label_node(m).element = "Waterfall3D";
        },
        "");
    expect(
        "custom element with a child",
        [](auto& m) {
            label_node(m).text.reset();
            label_node(m).kind = ir::NodeKind::CustomElement;
            label_node(m).element = "Waterfall3D";
            label_node(m).children.push_back(leaf(ir::NodeKind::View, "node::90", "90"));
        },
        "TESSERA_IR_LEAF;");
    expect(
        "text input with a child",
        [](auto& m) {
            label_node(m).text.reset();
            label_node(m).kind = ir::NodeKind::TextInput;
            label_node(m).children.push_back(leaf(ir::NodeKind::View, "node::90", "90"));
        },
        "TESSERA_IR_LEAF;");
    expect(
        "text area with a child",
        [](auto& m) {
            label_node(m).text.reset();
            label_node(m).kind = ir::NodeKind::TextArea;
            label_node(m).children.push_back(leaf(ir::NodeKind::View, "node::90", "90"));
        },
        "TESSERA_IR_LEAF;");
    expect(
        "a View may have children",
        [](auto& m) {
            list(m).root.children.push_back(leaf(ir::NodeKind::View, "node::90", "90"));
        },
        "");
    expect(
        "component node of an unknown component",
        [](auto& m) { row_node(m).component = "comp::Gone"; }, kUnresolved);
    expect(
        "component node with slot children is allowed",
        [](auto& m) { row_node(m).children.push_back(leaf(ir::NodeKind::View, "node::90", "90")); },
        "");
    expect(
        "component reference on a View", [](auto& m) { label_node(m).component = "comp::Row"; },
        kMalformed);
    expect(
        "component node pointing at its own component",
        [](auto& m) { row_node(m).component = "comp::List"; }, "TESSERA_IR_SLOT;TESSERA_IR_CYCLE;");
    expect(
        "component reference cycle across two components",
        [](auto& m) {
            ir::Node reference = leaf(ir::NodeKind::Component, "node::92", "92");
            reference.component = "comp::List";
            m.components[1].root.children.push_back(std::move(reference));
        },
        "TESSERA_IR_CYCLE;");
}

void test_error_boundary() {
    const auto boundary = [](ir::IrModule& m) -> ir::Node& { return list(m).root.children[1]; };
    expect(
        "no fallback", [&](auto& m) { boundary(m).fallback.reset(); },
        "TESSERA_IR_ERROR_BOUNDARY;");
    expect(
        "two children",
        [&](auto& m) {
            boundary(m).children.push_back(leaf(ir::NodeKind::View, "node::90", "90"));
        },
        "TESSERA_IR_ERROR_BOUNDARY;");
    expect(
        "no child", [&](auto& m) { boundary(m).children.clear(); },
        "TESSERA_IR_ERROR_BOUNDARY;TESSERA_IR_BINDING;");
    expect(
        "fallback on a View",
        [&](auto& m) {
            label_node(m).fallback =
                std::make_unique<ir::Node>(leaf(ir::NodeKind::View, "node::90", "90"));
        },
        kMalformed);
    expect(
        "fallback subtree is validated too", [&](auto& m) { boundary(m).fallback->ui_id.clear(); },
        kMissing);
}

void test_loops() {
    expect("loop without a key", [](auto& m) { row_node(m).key.reset(); }, "TESSERA_IR_LIST_KEY;");
    expect(
        "loop source is not a reference",
        [](auto& m) { row_node(m).loop->source = ir::make_literal(std::int64_t{1}); },
        kUnsupported);
    expect(
        "loop without an item name", [](auto& m) { row_node(m).loop->item.clear(); },
        std::string(kMalformed) + kUnresolved + kUnresolved);
    expect(
        "literal key outside a loop is fine",
        [](auto& m) { label_node(m).key = ir::make_literal(std::string("k")); }, "");
    expect(
        "expression key outside a loop",
        [](auto& m) { label_node(m).key = ir::make_ref("state::n"); }, kMalformed);
    expect(
        "two nested loops are allowed",
        [](auto& m) {
            ir::Node inner = leaf(ir::NodeKind::View, "node::90", "90");
            inner.key = ir::make_ref("inner");
            inner.loop = ir::Loop{"inner", "", ir::make_ref("item.children")};
            row_node(m).kind = ir::NodeKind::View;
            row_node(m).component.clear();
            row_node(m).children.push_back(std::move(inner));
        },
        "");
    expect(
        "three nested loops",
        [](auto& m) {
            ir::Node innermost = leaf(ir::NodeKind::View, "node::91", "91");
            innermost.key = ir::make_ref("c");
            innermost.loop = ir::Loop{"c", "", ir::make_ref("inner.items")};
            ir::Node inner = leaf(ir::NodeKind::View, "node::90", "90");
            inner.key = ir::make_ref("inner");
            inner.loop = ir::Loop{"inner", "", ir::make_ref("item.children")};
            inner.children.push_back(std::move(innermost));
            row_node(m).kind = ir::NodeKind::View;
            row_node(m).component.clear();
            row_node(m).children.push_back(std::move(inner));
        },
        "TESSERA_IR_LOOP;");
    expect(
        "sibling loops do not add up",
        [](auto& m) {
            for (int i = 0; i < 3; ++i) {
                ir::Node next = leaf(ir::NodeKind::View, "node::8" + std::to_string(i),
                                     "8" + std::to_string(i));
                next.key = ir::make_ref("x");
                next.loop = ir::Loop{"x", "", ir::make_ref("state::rows")};
                list(m).root.children.push_back(std::move(next));
            }
        },
        "");
}

void test_virtual_collection() {
    const auto make_template = [] {
        ir::Node item = leaf(ir::NodeKind::View, "node::91", "91");
        item.key = ir::make_ref("item");
        item.loop = ir::Loop{"item", "index", ir::make_ref("state::rows")};
        return std::make_unique<ir::Node>(std::move(item));
    };
    const auto as_collection = [&](ir::IrModule& m, ir::NodeKind kind) -> ir::Node& {
        ir::Node& node = label_node(m);
        node.text.reset();
        node.kind = kind;
        node.item_template = make_template();
        return node;
    };
    expect(
        "a virtual list with a looped template",
        [&](auto& m) { as_collection(m, ir::NodeKind::VirtualList); }, "");
    expect(
        "a virtual grid with a looped template",
        [&](auto& m) { as_collection(m, ir::NodeKind::VirtualGrid); }, "");
    expect(
        "a virtual list without a template",
        [&](auto& m) { as_collection(m, ir::NodeKind::VirtualList).item_template.reset(); },
        "TESSERA_IR_VIRTUAL;");
    expect(
        "a template without a loop",
        [&](auto& m) {
            ir::Node& tmpl = *as_collection(m, ir::NodeKind::VirtualList).item_template;
            tmpl.loop.reset();
            tmpl.key = ir::make_literal(std::string("k"));
        },
        "TESSERA_IR_VIRTUAL;");
    expect(
        "a template without an item key",
        [&](auto& m) { as_collection(m, ir::NodeKind::VirtualList).item_template->key.reset(); },
        "TESSERA_IR_LIST_KEY;");
    expect(
        "a conditional template",
        [&](auto& m) {
            as_collection(m, ir::NodeKind::VirtualList).item_template->condition =
                ir::make_ref("state::n");
        },
        "TESSERA_IR_VIRTUAL;");
    expect(
        "a virtual list with children",
        [&](auto& m) {
            as_collection(m, ir::NodeKind::VirtualList)
                .children.push_back(leaf(ir::NodeKind::View, "node::90", "90"));
        },
        "TESSERA_IR_VIRTUAL;");
    expect(
        "an itemTemplate on a View", [&](auto& m) { as_collection(m, ir::NodeKind::View); },
        kMalformed);
    expect(
        "the template subtree is validated too",
        [&](auto& m) { as_collection(m, ir::NodeKind::VirtualList).item_template->ui_id.clear(); },
        kMissing);
}

void test_slots() {
    // Section 3.4: the `children` prop declares the slot; `Row` in the fixture declares it.
    const auto row_root = [](ir::IrModule& m) -> ir::Node& { return m.components[1].root; };
    expect(
        "component children without a `children` prop",
        [&](auto& m) {
            m.components[1].props.pop_back();
            m.components[1].root.children.clear();
        },
        "TESSERA_IR_SLOT;");
    expect(
        "the `children` prop must have the children type",
        [&](auto& m) { m.components[1].props.back().type = "string"; },
        "TESSERA_IR_SLOT;TESSERA_IR_SLOT;TESSERA_IR_SLOT;");
    expect(
        "a second Slot",
        [&](auto& m) {
            row_root(m).children.push_back(leaf(ir::NodeKind::Slot, "node::90", "90"));
        },
        "TESSERA_IR_SLOT;");
    expect(
        "a Slot with props",
        [&](auto& m) { row_root(m).children[0].props.emplace("x", ir::make_literal(1)); },
        "TESSERA_IR_SLOT;");
    expect(
        "a Slot as the component root",
        [&](auto& m) { m.components[1].root = leaf(ir::NodeKind::Slot, "node::2", "11"); },
        "TESSERA_IR_SLOT;");
    expect(
        "a Slot inside another component's slot content",
        [&](auto& m) {
            m.components[0].props.push_back({"children", "children", nullptr});
            ir::Node& content_root = row_node(m);
            content_root.loop.reset();
            content_root.key.reset();
            content_root.condition.reset();
            content_root.props.clear();
            content_root.children.push_back(leaf(ir::NodeKind::Slot, "node::91", "91"));
        },
        "TESSERA_IR_SLOT;");
    expect(
        "a Slot inside a loop template",
        [&](auto& m) {
            m.components[1].state.push_back(
                {"state::rows", "rows", "list<int>", ir::make_array({})});
            row_root(m).loop = ir::Loop{"item", "", ir::make_ref("state::rows")};
            row_root(m).key = ir::make_literal(std::string("k"));
        },
        "TESSERA_IR_SLOT;");
}

void test_spans_and_order() {
    const std::string bad_span = "TESSERA_IR_SPAN;";
    expect("component without a file", [](auto& m) { list(m).source.file.clear(); }, bad_span);
    expect("node without a file", [](auto& m) { label_node(m).source.file.clear(); }, bad_span);
    expect("node with line 0", [](auto& m) { label_node(m).source.line = 0; }, bad_span);
    expect("node with column 0", [](auto& m) { label_node(m).source.column = 0; }, bad_span);
    expect(
        "end line before start line",
        [](auto& m) {
            label_node(m).source.line = 5;
            label_node(m).source.end_line = 4;
        },
        bad_span);
    expect(
        "end column before start on one line",
        [](auto& m) {
            label_node(m).source.line = 5;
            label_node(m).source.end_line = 5;
            label_node(m).source.column = 9;
            label_node(m).source.end_column = 8;
        },
        bad_span);
    expect(
        "an empty span on one line is fine",
        [](auto& m) {
            label_node(m).source.end_line = label_node(m).source.line;
            label_node(m).source.end_column = label_node(m).source.column;
        },
        "");
    expect(
        "effect without a span", [](auto& m) { list(m).effects[0].source.file.clear(); }, bad_span);
    expect(
        "handler without a span", [](auto& m) { list(m).handlers[0].source.file.clear(); },
        bad_span);
    expect(
        "environment without a span", [](auto& m) { list(m).environment[0].source.file.clear(); },
        bad_span);
    expect("binding without a span", [](auto& m) { m.bindings[0].source.file.clear(); }, bad_span);

    expect(
        "components out of order", [](auto& m) { std::swap(m.components[0], m.components[1]); },
        "TESSERA_IR_ORDER;");
    expect(
        "components ordered by file first",
        [](auto& m) {
            m.components[0].source.file = "ui/Z.tsx";
            m.components[0].root.source.file = "ui/Z.tsx";
        },
        "TESSERA_IR_ORDER;");
    expect(
        "equal component names in one file are out of order",
        [](auto& m) { m.components[1].name = "List"; }, "TESSERA_IR_ORDER;");
    expect(
        "bindings out of order", [](auto& m) { std::swap(m.bindings[0], m.bindings[1]); },
        "TESSERA_IR_ORDER;");
}

} // namespace

int main() {
    test_node_shapes();
    test_error_boundary();
    test_loops();
    test_slots();
    test_virtual_collection();
    test_spans_and_order();
    return finish("ir validate shape tests");
}
