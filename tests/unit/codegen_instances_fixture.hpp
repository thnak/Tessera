// SPDX-License-Identifier: MIT

// Hand-built IR for the per-branch and per-item instance fixtures (issue #231,
// docs/design/compiler-node-state.md). Shared by the codegen emission test (which owns the stored
// goldens through `--write`) and the runtime test that compiles those goldens.
//
// `Row` owns state and interpolates it, and maps a list-typed prop:
//   <View className="row">
//     <Text>{label}:{count}</Text>
//     {notes.map((note) => <Text key={note}>{note}</Text>)}
//   </View>
// `Page` exercises all four lifted forms:
//   <View>
//     {nums.map((n, i) => <View key={i}>{i > 0 && <Text>{n}</Text>}</View>)}      // presence per
//     item {entries.map((entry) => <Row key={entry} label={entry} notes={[entry]} />)} // keyed
//     instances {show && <Row label="cond" notes={[]} />}                                   //
//     conditional instance
//   </View>

#pragma once

#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace instances_fixture {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t length) {
    ir::SourceSpan value;
    value.file = "ui/Instances.tsx";
    value.offset = offset;
    value.length = length;
    value.line = 1;
    value.column = offset + 1;
    value.end_line = 1;
    value.end_column = offset + length + 1;
    return value;
}

inline ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

inline ir::ExprPtr number(std::int64_t value) {
    return ir::make_literal(value);
}

inline ir::ExprPtr ref(std::string id) {
    return ir::make_ref(std::move(id));
}

inline ir::ExprPtr array_of(ir::ExprPtr element) {
    std::vector<ir::ExprPtr> elements;
    elements.push_back(std::move(element));
    return ir::make_array(std::move(elements));
}

inline ir::ExprPtr string_array(std::initializer_list<const char*> values) {
    std::vector<ir::ExprPtr> elements;
    for (const char* value : values) {
        elements.push_back(text(value));
    }
    return ir::make_array(std::move(elements));
}

inline ir::ExprPtr int_array(std::initializer_list<std::int64_t> values) {
    std::vector<ir::ExprPtr> elements;
    for (const std::int64_t value : values) {
        elements.push_back(number(value));
    }
    return ir::make_array(std::move(elements));
}

inline ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id,
                        std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(offset, 8);
    return node;
}

inline ir::Node text_node(std::string id, std::string ui_id, std::uint32_t offset,
                          ir::ExprPtr value) {
    ir::Node node = node_of(ir::NodeKind::Text, std::move(id), std::move(ui_id), offset);
    node.text = std::move(value);
    return node;
}

inline ir::Component make_row_component() {
    ir::Component row;
    row.id = "comp::Row";
    row.component_id = "9001";
    row.name = "Row";
    row.source = span(0, 120);
    row.props.push_back({"label", "string", nullptr, false});
    row.props.push_back({"notes", "list<string>", nullptr, false});
    row.state.push_back({"state::count", "count", "int", number(0)});

    row.root = node_of(ir::NodeKind::View, "node::1", "1101", 10);
    row.root.props.emplace("className", text("row"));
    // `{label}:{count}` as one template so the state cell is visible in the painted text.
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::expr_part(ref("prop::label")));
    parts.push_back(ir::text_part(":"));
    parts.push_back(ir::expr_part(ref("state::count")));
    row.root.children.push_back(
        text_node("node::2", "1102", 20, ir::make_template(std::move(parts))));

    // The list-typed prop iterated inside the callee: `notes.map((note) => <Text>{note}</Text>)`.
    ir::Node notes = node_of(ir::NodeKind::View, "node::3", "1103", 30);
    notes.loop = ir::Loop{"note", std::string(), ref("prop::notes")};
    notes.key = ref("note");
    notes.children.push_back(text_node("node::4", "1104", 40, ref("note")));
    row.root.children.push_back(std::move(notes));
    return row;
}

inline ir::Component make_page_component() {
    ir::Component page;
    page.id = "comp::Page";
    page.component_id = "9002";
    page.name = "Page";
    page.source = span(200, 160);
    page.state.push_back({"state::show", "show", "bool", ir::make_literal(false)});
    page.state.push_back(
        {"state::entries", "entries", "list<string>", string_array({"a", "b", "c"})});
    page.state.push_back({"state::nums", "nums", "list<int>", int_array({10, 20})});

    page.root = node_of(ir::NodeKind::View, "node::1", "2101", 210);

    // A presence slot per item: `{nums.map((n, i) => <View key={i}>{i > 0 &&
    // <Text>{n}</Text>}</View>)}`.
    ir::Node per_item = node_of(ir::NodeKind::View, "node::2", "2102", 220);
    per_item.loop = ir::Loop{"n", "i", ref("state::nums")};
    per_item.key = ref("i");
    ir::Node presence = text_node("node::3", "2103", 230, ref("n"));
    presence.condition = ir::make_binary(">", ref("i"), number(0));
    per_item.children.push_back(std::move(presence));
    page.root.children.push_back(std::move(per_item));

    // A keyed list of component instances: the template root is the `Row` instance itself.
    ir::Node rows = node_of(ir::NodeKind::Component, "node::4", "2104", 240);
    rows.component = "comp::Row";
    rows.automation_id = "2104"; // the automation identity is `(template uiId, item key)`
    rows.loop = ir::Loop{"entry", std::string(), ref("state::entries")};
    rows.key = ref("entry");
    rows.props.emplace("label", ref("entry"));
    rows.props.emplace("notes", array_of(ref("entry")));
    page.root.children.push_back(std::move(rows));

    // A conditional instance: `{show && <Row label="cond" notes={[]} />}`.
    ir::Node conditional = node_of(ir::NodeKind::Component, "node::5", "2105", 250);
    conditional.component = "comp::Row";
    conditional.condition = ref("state::show");
    conditional.props.emplace("label", text("cond"));
    std::vector<ir::ExprPtr> none;
    conditional.props.emplace("notes", ir::make_array(std::move(none)));
    page.root.children.push_back(std::move(conditional));
    return page;
}

inline ir::IrModule make_instances_module() {
    ir::IrModule module;
    module.name = "demo";
    // ir-v1 section 2 orders components by source file then name: Page before Row.
    ir::Component page = make_page_component();
    module.components.push_back(std::move(page));
    ir::Component row = make_row_component();
    module.components.push_back(std::move(row));
    return module;
}

} // namespace instances_fixture
