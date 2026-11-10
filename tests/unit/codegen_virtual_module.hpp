// SPDX-License-Identifier: MIT
//
// Virtual-collection IR fixture for the codegen tests: a VirtualList and a VirtualGrid, each with a
// static item template, plus a protected-region binding next to them.

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::codegen_virtual {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                           std::uint32_t column) {
    return ir::SourceSpan{"ui/VirtualDemo.tsx", offset, length, line, column, line,
                          column + length};
}

inline ir::ExprPtr integer(std::int64_t value) {
    return ir::make_literal(value);
}

inline ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(10, 5, 1, 1);
    return node;
}

// `{item} # {count}`: the item text depends on the loop item and on component state, so a
// content-only change is observable without a structural change.
inline ir::ExprPtr item_text() {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::expr_part(ir::make_ref("item")));
    parts.push_back(ir::text_part(" # "));
    parts.push_back(ir::expr_part(ir::make_ref("state::count")));
    return ir::make_template(std::move(parts));
}

inline ir::ExprPtr grid_text() {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::expr_part(ir::make_ref("cell")));
    return ir::make_template(std::move(parts));
}

inline ir::IrModule make_virtual_module() {
    ir::IrModule module;
    module.name = "vlist";

    ir::Component component;
    component.id = "comp::VirtualDemo";
    component.component_id = "1";
    component.name = "VirtualDemo";
    component.source = span(0, 240, 1, 1);
    // A list-typed prop seeds the list state: the IR-expressible list initial value.
    component.props.push_back({"seed", "list<string>", nullptr});
    component.state.push_back({"state::count", "count", "int", integer(0)});
    component.state.push_back(
        {"state::items", "items", "list<string>", ir::make_ref("prop::seed")});

    ir::Node root = leaf(ir::NodeKind::View, "node::1", "1");
    root.children.push_back(leaf(ir::NodeKind::Text, "node::2", "2"));
    root.children.back().text = ir::make_literal(std::string("Header"));

    // `{rows.map((item) => <View key={item}><Text>{item} # {count}</Text></View>)}` inside a
    // VirtualList: the item template is not a child, so its nodes take the next ordinals.
    ir::Node list = leaf(ir::NodeKind::VirtualList, "node::3", "3");
    list.props.emplace("estimateSize", integer(32));
    list.props.emplace("gap", integer(4));
    // Collection-level events (virtualization-v1.md section 10): an `onScroll` binding receives the
    // scene ScrollEvent payload, an `onEndReached` inline handler takes none.
    list.events.emplace("onScroll", "bind::on_scroll");
    list.events.emplace("onEndReached", "handler::on_end_reached");
    ir::Node list_item = leaf(ir::NodeKind::View, "node::4", "4");
    list_item.key = ir::make_ref("item");
    list_item.loop = ir::Loop{"item", "", ir::make_ref("state::items")};
    ir::Node list_text = leaf(ir::NodeKind::Text, "node::5", "5");
    list_text.text = item_text();
    list_item.children.push_back(std::move(list_text));
    list.item_template = std::make_unique<ir::Node>(std::move(list_item));
    root.children.push_back(std::move(list));

    // A VirtualGrid over the same data: fixed row height and column count.
    ir::Node grid = leaf(ir::NodeKind::VirtualGrid, "node::6", "6");
    grid.props.emplace("rowHeight", integer(40));
    grid.props.emplace("columns", integer(2));
    grid.props.emplace("gap", integer(2));
    ir::Node grid_item = leaf(ir::NodeKind::View, "node::7", "7");
    grid_item.key = ir::make_ref("cell");
    grid_item.loop = ir::Loop{"cell", "", ir::make_ref("state::items")};
    ir::Node grid_text_node = leaf(ir::NodeKind::Text, "node::8", "8");
    grid_text_node.text = grid_text();
    grid_item.children.push_back(std::move(grid_text_node));
    grid.item_template = std::make_unique<ir::Node>(std::move(grid_item));
    root.children.push_back(std::move(grid));

    ir::Node footer = leaf(ir::NodeKind::Text, "node::9", "9");
    footer.text = ir::make_literal(std::string("Footer"));
    root.children.push_back(std::move(footer));

    // A protected-region binding next to the collections (merger idempotency coverage).
    ir::Node button = leaf(ir::NodeKind::Button, "node::10", "10");
    button.events.emplace("onClick", "bind::on_click");
    ir::Node add_text = leaf(ir::NodeKind::Text, "node::11", "11");
    add_text.text = ir::make_literal(std::string("Add"));
    button.children.push_back(std::move(add_text));
    root.children.push_back(std::move(button));

    component.root = std::move(root);

    // The inline `onEndReached={() => setCount(...)}` handler lowered to a zero-payload record.
    ir::Handler on_end_reached;
    on_end_reached.id = "handler::on_end_reached";
    on_end_reached.source = span(120, 20, 8, 3);
    on_end_reached.actions.push_back(
        ir::Action{ir::ActionKind::Set, "state::count", ir::make_literal(std::int64_t{1})});
    component.handlers.push_back(std::move(on_end_reached));
    module.components.push_back(std::move(component));

    ir::Binding binding;
    binding.id = "bind::on_click";
    binding.component = "comp::VirtualDemo";
    binding.kind = ir::BindingKind::Event;
    binding.region = "on_click";
    binding.source = span(40, 10, 4, 3);
    binding.params.push_back({"event", "ClickEvent"});
    module.bindings.push_back(std::move(binding));

    // The `onScroll={(e: ScrollEvent) => ...}` binding: the scene scroll payload record.
    ir::Binding scroll;
    scroll.id = "bind::on_scroll";
    scroll.component = "comp::VirtualDemo";
    scroll.kind = ir::BindingKind::Event;
    scroll.region = "on_scroll";
    scroll.source = span(140, 10, 9, 3);
    scroll.params.push_back({"e", "ScrollEvent"});
    module.bindings.push_back(std::move(scroll));
    return module;
}

} // namespace tessera::test::codegen_virtual
