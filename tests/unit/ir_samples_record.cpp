// SPDX-License-Identifier: MIT

// The record-type fixture of docs/specs/ir-v1.md section 2.2 (ADR-051): a module with a type table
// and every position a record value is allowed in.

#include "ir_samples.hpp"

#include <cstdint>
#include <utility>

namespace ir_test {

namespace {

ir::RecordType record_type(std::string name, std::vector<ir::RecordMember> members,
                           ir::SourceSpan source) {
    ir::RecordType type;
    type.id = "type::" + name;
    type.name = std::move(name);
    type.members = std::move(members);
    type.source = std::move(source);
    return type;
}

ir::ExprPtr size_record(std::int64_t w, std::int64_t h) {
    std::vector<ir::RecordField> fields;
    fields.push_back({"w", ir::make_literal(w)});
    fields.push_back({"h", ir::make_literal(h)});
    return ir::make_record("Size", std::move(fields));
}

std::vector<ir::ExprPtr> literal_list(std::int64_t first, std::int64_t second) {
    std::vector<ir::ExprPtr> items;
    items.push_back(ir::make_literal(first));
    items.push_back(ir::make_literal(second));
    return items;
}

// `Style { color, sizes: list<int>, padding: Size?, label: string? }`.
ir::ExprPtr style_record() {
    std::vector<ir::RecordField> fields;
    fields.push_back({"color", ir::make_literal(std::string("#0b57d0"))});
    fields.push_back({"sizes", ir::make_array(literal_list(1, 2))});
    // `padding` is optional and written here as a nested record; `label` is omitted.
    fields.push_back({"padding", size_record(4, 8)});
    return ir::make_record("Style", std::move(fields));
}

ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id, ir::SourceSpan source) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = std::move(source);
    return node;
}

} // namespace

ir::IrModule make_record_module() {
    ir::IrModule module;
    module.name = "records";
    // Sorted by name: Size before Style (section 11).
    module.types.push_back(
        record_type("Size", {{"w", "int", false}, {"h", "int", false}}, span(0, 40, 1, 1, 1, 41)));
    module.types.push_back(record_type("Style",
                                       {{"color", "string", false},
                                        {"sizes", "list<int>", false},
                                        {"padding", "Size", true},
                                        {"label", "string", true}},
                                       span(42, 90, 2, 1, 6, 2)));

    ir::Component card;
    card.id = "comp::Card";
    card.component_id = "77";
    card.name = "Card";
    card.source = span(200, 300, 10, 1, 24, 2);
    // A prop default of record type (one of the three allowed record positions).
    card.props.push_back({"size", "Size", size_record(10, 20)});
    // A nested record and a list of records in a prop default.
    card.props.push_back({"style", "Style", style_record()});
    card.props.push_back({"label", "string", ir::make_literal(std::string("card"))});
    // A `useState` initial of record type (the second allowed position).
    card.state.push_back({"state::size", "size", "Size", size_record(1, 2)});
    card.root = leaf(ir::NodeKind::View, "node::1", "1", span(220, 200, 11, 3, 22, 9));
    ir::Node text = leaf(ir::NodeKind::Text, "node::2", "2", span(230, 90, 12, 5, 12, 95));
    // A dotted `ref` into a declared record: `size.w` is a member, so it resolves.
    text.text = ir::make_ref("prop::size.w");
    card.root.children.push_back(std::move(text));
    module.components.push_back(std::move(card));
    return module;
}

ir::IrModule make_no_record_module() {
    ir::IrModule module;
    module.name = "records";
    ir::Component card;
    card.id = "comp::Card";
    card.component_id = "77";
    card.name = "Card";
    card.source = span(200, 300, 10, 1, 24, 2);
    card.props.push_back({"label", "string", ir::make_literal(std::string("card"))});
    card.root = leaf(ir::NodeKind::View, "node::1", "1", span(220, 200, 11, 3, 22, 9));
    ir::Node text = leaf(ir::NodeKind::Text, "node::2", "2", span(230, 90, 12, 5, 12, 95));
    text.text = ir::make_literal(std::string("card"));
    card.root.children.push_back(std::move(text));
    module.components.push_back(std::move(card));
    return module;
}

} // namespace ir_test
