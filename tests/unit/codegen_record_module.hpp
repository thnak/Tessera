// SPDX-License-Identifier: MIT
//
// IR fixture for the generated record equality and per-field dirty bits (issue #834, ADR-051
// implementation note, ADR-056 decisions 1 and 5):
//
//   - `Form { name: string; email: string; age: int }` is a record state cell.
//   - `FormPage` renders `{form.name}` in one Text and `{form.email}` in another, so a change to
//     `email` must re-run no binding that reads only `name`.
//   - `Entry { id: string; label: string }` is a keyed `list<Entry>` source, rendered by a Text
//     that reads only `row.label`; the key is `row.id`. Editing one row's `label` repaints that item
//     only.
//
// The shape is hand-built (the IR is what codegen consumes); the emitted bytes and the runtime
// behaviour are the two things under test.

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::codegen_record {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset) {
    return ir::SourceSpan{"ui/Records.tsx", offset, 10, 1, offset + 1, 1, offset + 11};
}

inline ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id,
                        std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(offset);
    return node;
}

inline ir::ExprPtr ref(std::string id) {
    return ir::make_ref(std::move(id));
}

inline ir::ExprPtr text_of(std::string prefix, std::string id) {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part(std::move(prefix)));
    parts.push_back(ir::expr_part(ref(std::move(id))));
    return ir::make_template(std::move(parts));
}

// `Form{...}` with the given field values, in member order (name, email, age).
inline ir::ExprPtr form_record(std::string name, std::string email, std::int64_t age) {
    std::vector<ir::RecordField> fields;
    fields.push_back(ir::RecordField{"name", ir::make_literal(std::move(name))});
    fields.push_back(ir::RecordField{"email", ir::make_literal(std::move(email))});
    fields.push_back(ir::RecordField{"age", ir::make_literal(age)});
    return ir::make_record("Form", std::move(fields));
}

// `Entry{id, label}` in member order.
inline ir::ExprPtr entry_record(std::string id, std::string label) {
    std::vector<ir::RecordField> fields;
    fields.push_back(ir::RecordField{"id", ir::make_literal(std::move(id))});
    fields.push_back(ir::RecordField{"label", ir::make_literal(std::move(label))});
    return ir::make_record("Entry", std::move(fields));
}

// `row.label` (a dotted read of the loop item).
inline ir::ExprPtr row_field_text(std::string prefix) {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part(std::move(prefix)));
    parts.push_back(ir::expr_part(ref("row.label")));
    return ir::make_template(std::move(parts));
}

inline ir::IrModule make_record_module() {
    ir::IrModule module;
    module.name = "demo";
    // `types` is sorted by name: Entry < Form.
    module.types.push_back(ir::RecordType{
        "type::Entry", "Entry", {{"id", "string", false}, {"label", "string", false}}, span(1)});
    module.types.push_back(ir::RecordType{
        "type::Form",
        "Form",
        {{"name", "string", false}, {"email", "string", false}, {"age", "int", false}},
        span(0)});

    // FormPage: a record state cell, two field reads, and a keyed list of records.
    ir::Component page;
    page.id = "comp::FormPage";
    page.component_id = "1";
    page.name = "FormPage";
    page.source = span(200);
    // The list is seeded from a prop, the only IR-expressible list initial value.
    page.props.push_back({"seed", "list<Entry>", nullptr});
    page.state.push_back(
        {"state::form", "form", "Form", form_record("Ann", "ann@example.com", 30)});
    page.state.push_back({"state::rows", "rows", "list<Entry>", ir::make_ref("prop::seed")});

    page.root = node_of(ir::NodeKind::View, "node::1", "2101", 210);
    ir::Node name_text = node_of(ir::NodeKind::Text, "node::2", "2102", 220);
    name_text.text = text_of("name=", "state::form.name");
    page.root.children.push_back(std::move(name_text));
    ir::Node email_text = node_of(ir::NodeKind::Text, "node::3", "2103", 230);
    email_text.text = text_of("email=", "state::form.email");
    page.root.children.push_back(std::move(email_text));

    ir::Node rows = node_of(ir::NodeKind::View, "node::4", "2104", 240);
    rows.loop = ir::Loop{"row", std::string(), ref("state::rows")};
    rows.key = ref("row.id");
    ir::Node row_text = node_of(ir::NodeKind::Text, "node::5", "2105", 250);
    row_text.text = row_field_text("label=");
    rows.children.push_back(std::move(row_text));
    page.root.children.push_back(std::move(rows));

    // A handler that writes only `name` via a record update (`{...form, name: x}`), and one that
    // sets the whole record, so the per-field and whole-record setters are both exercised.
    ir::Handler set_name;
    set_name.id = "handler::1";
    set_name.source = span(260);
    ir::Action name_action;
    name_action.kind = ir::ActionKind::Set;
    name_action.target = "state::form";
    {
        std::vector<ir::RecordField> fields;
        fields.push_back(ir::RecordField{"name", ir::make_literal(std::string("Bob"))});
        name_action.value = ir::make_record_update("Form", "state::form", std::move(fields));
    }
    set_name.actions.push_back(std::move(name_action));
    page.handlers.push_back(std::move(set_name));

    ir::Handler set_all;
    set_all.id = "handler::2";
    set_all.source = span(270);
    ir::Action all_action;
    all_action.kind = ir::ActionKind::Set;
    all_action.target = "state::form";
    all_action.value = form_record("Cid", "cid@example.com", 41);
    set_all.actions.push_back(std::move(all_action));
    page.handlers.push_back(std::move(set_all));

    module.components.push_back(std::move(page));
    return module;
}

} // namespace tessera::test::codegen_record
