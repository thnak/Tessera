// SPDX-License-Identifier: MIT
//
// Prop-driven virtual-collection IR fixture for the dynamic structural-option codegen tests: a
// VirtualList whose `orientation` and `overscan` come from component props, and a VirtualGrid whose
// `columns` comes from a prop, over one 10-item string source. The literal fixture in
// `codegen_virtual_module.hpp` stays untouched so its committed golden must not move.

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::codegen_dynamic_virtual {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                           std::uint32_t column) {
    return ir::SourceSpan{"ui/DynamicVirtual.tsx", offset, length, line, column, line,
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

// `{item}`: the item text depends on the loop item, so the per-item content pass is exercised.
inline ir::ExprPtr item_text(std::string name) {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::expr_part(ir::make_ref(std::move(name))));
    return ir::make_template(std::move(parts));
}

inline ir::ExprPtr make_items() {
    std::vector<ir::ExprPtr> rows;
    for (int index = 0; index < 10; ++index) {
        rows.push_back(ir::make_literal("row-" + std::to_string(index)));
    }
    return ir::make_array(std::move(rows));
}

inline ir::IrModule make_dynamic_virtual_module() {
    ir::IrModule module;
    module.name = "dvirt";

    ir::Component component;
    component.id = "comp::DynamicVirtual";
    component.component_id = "3";
    component.name = "DynamicVirtual";
    component.source = span(0, 240, 1, 1);
    // The structural options under test: a component prop reference, not a literal.
    component.props.push_back({"orientation", "string", nullptr});
    component.props.push_back({"overscan", "int", nullptr});
    component.props.push_back({"columns", "int", nullptr});
    component.props.push_back({"columnWidth", "float", nullptr});
    component.state.push_back({"state::items", "items", "list<string>", make_items()});

    ir::Node root = leaf(ir::NodeKind::View, "node::1", "1");
    ir::Node header = leaf(ir::NodeKind::Text, "node::2", "2");
    header.text = ir::make_literal(std::string("Header"));
    root.children.push_back(std::move(header));

    // `orientation={props.orientation}` and `overscan={props.overscan}`: both refresh at sync time.
    ir::Node list = leaf(ir::NodeKind::VirtualList, "node::3", "3");
    list.props.emplace("orientation", ir::make_ref("prop::orientation"));
    list.props.emplace("overscan", ir::make_ref("prop::overscan"));
    list.props.emplace("itemSize", integer(100));
    ir::Node list_item = leaf(ir::NodeKind::View, "node::4", "4");
    list_item.key = ir::make_ref("item");
    list_item.loop = ir::Loop{"item", "", ir::make_ref("state::items")};
    ir::Node list_text = leaf(ir::NodeKind::Text, "node::5", "5");
    list_text.text = item_text("item");
    list_item.children.push_back(std::move(list_text));
    list.item_template = std::make_unique<ir::Node>(std::move(list_item));
    root.children.push_back(std::move(list));

    // `columns={props.columns}` and `columnWidth={props.columnWidth}`: the row mapping and the
    // column-width mode are prop-driven; the literal row height and overscan isolate them.
    ir::Node grid = leaf(ir::NodeKind::VirtualGrid, "node::6", "6");
    grid.props.emplace("columns", ir::make_ref("prop::columns"));
    grid.props.emplace("columnWidth", ir::make_ref("prop::columnWidth"));
    grid.props.emplace("rowHeight", integer(100));
    grid.props.emplace("overscan", integer(0));
    ir::Node grid_item = leaf(ir::NodeKind::View, "node::7", "7");
    grid_item.key = ir::make_ref("cell");
    grid_item.loop = ir::Loop{"cell", "", ir::make_ref("state::items")};
    ir::Node grid_text = leaf(ir::NodeKind::Text, "node::8", "8");
    grid_text.text = item_text("cell");
    grid_item.children.push_back(std::move(grid_text));
    grid.item_template = std::make_unique<ir::Node>(std::move(grid_item));
    root.children.push_back(std::move(grid));

    ir::Node footer = leaf(ir::NodeKind::Text, "node::9", "9");
    footer.text = ir::make_literal(std::string("Footer"));
    root.children.push_back(std::move(footer));

    component.root = std::move(root);
    module.components.push_back(std::move(component));
    return module;
}

} // namespace tessera::test::codegen_dynamic_virtual
