// SPDX-License-Identifier: MIT

// Shared hand-built IR fixtures for the `Field`/`Form` codegen tests
// (docs/specs/forms-and-validation-v1.md sections 5.1 and 5.2). The module carries the real library
// declarations (`lowering::make_library_component`) plus two project pages:
//
// - `FieldPage`: one standalone `Field` with an explicit controlled value and an `onValidate`
//   callback, the pre-`Form` usage (task-90 coverage).
// - `FormPage`: the library `Form` with `initial`/`validate`/`onSubmit`, two Fields inside its
//   slot and their TextInputs, the generated form-state binding (task-103).
//
// The goldens are generated from this IR by `forms_codegen_tests --write`.

#pragma once

#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/lowering/library_components.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forms_codegen_fixture {

namespace ir = tessera::compiler::ir;
namespace lowering = tessera::compiler::lowering;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                           std::uint32_t column, std::uint32_t end_column) {
    ir::SourceSpan value;
    value.file = "ui/FormPage.tsx";
    value.offset = offset;
    value.length = length;
    value.line = line;
    value.column = column;
    value.end_line = line;
    value.end_column = end_column;
    return value;
}

inline ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

// A string-array literal expression (an initializer_list of unique_ptr cannot be copied).
inline ir::ExprPtr string_array(std::initializer_list<std::string_view> values) {
    std::vector<ir::ExprPtr> items;
    items.reserve(values.size());
    for (const std::string_view value : values) {
        items.push_back(ir::make_literal(std::string(value)));
    }
    return ir::make_array(std::move(items));
}

inline ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id,
                        ir::SourceSpan source) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = std::move(source);
    return node;
}

inline ir::Node text_input(std::string id, std::string ui_id, ir::SourceSpan source) {
    ir::Node node = node_of(ir::NodeKind::TextInput, std::move(id), std::move(ui_id), source);
    return node;
}

inline ir::Binding event_binding(std::string id, std::string component, std::string region,
                                 ir::SourceSpan source) {
    ir::Binding binding;
    binding.id = std::move(id);
    binding.component = std::move(component);
    binding.kind = ir::BindingKind::Event;
    binding.region = std::move(region);
    binding.params = {};
    binding.source = std::move(source);
    return binding;
}

// One standalone `Field` with an explicit value expression and an `onValidate` callback.
inline ir::Component make_field_page() {
    ir::Component page;
    page.id = "comp::FieldPage";
    page.component_id = "70";
    page.name = "FieldPage";
    page.source = span(0, 420, 1, 1, 24);
    page.state.push_back({"state::email", "email", "string", text("")});

    page.root = node_of(ir::NodeKind::View, "node::1", "401", span(10, 380, 3, 3, 30));
    page.root.props.emplace("className", text("field-page"));

    ir::Node button = node_of(ir::NodeKind::Button, "node::2", "402", span(20, 20, 4, 5, 20));
    button.props.emplace("label", text("Set"));
    button.events.emplace("onClick", "handler::1");

    ir::Node field = node_of(ir::NodeKind::Component, "node::3", "403", span(30, 80, 5, 5, 80));
    field.component = "comp::Field";
    field.props.emplace("name", text("email"));
    field.props.emplace("label", text("Email"));
    field.props.emplace("value", ir::make_ref("state::email"));
    field.props.emplace("required", ir::make_literal(true));
    std::vector<ir::ExprPtr> validators;
    validators.push_back(text("required"));
    validators.push_back(text("minLength:3"));
    field.props.emplace("validators", ir::make_array(std::move(validators)));
    field.props.emplace("error", text(""));
    field.events.emplace("onValidate", "bind::on_validate");
    field.children.push_back(text_input("node::4", "404", span(40, 20, 6, 7, 20)));

    page.root.children.push_back(std::move(button));
    page.root.children.push_back(std::move(field));

    ir::Handler set_email;
    set_email.id = "handler::1";
    set_email.source = span(15, 18, 3, 8, 26);
    ir::Action set;
    set.kind = ir::ActionKind::Set;
    set.target = "state::email";
    set.value = text("ada@example.com");
    set_email.actions.push_back(std::move(set));
    page.handlers.push_back(std::move(set_email));
    return page;
}

// The form page: `<Form initial={["email=", "age=0"]} validate={...} onSubmit={...}>` with two
// Fields in its slot. The generated binding owns the controller in this component.
inline ir::Component make_form_page() {
    ir::Component page;
    page.id = "comp::FormPage";
    page.component_id = "71";
    page.name = "FormPage";
    page.source = span(0, 520, 1, 1, 24);

    page.root = node_of(ir::NodeKind::View, "node::1", "501", span(10, 500, 3, 3, 30));
    page.root.props.emplace("className", text("form-page"));

    ir::Node form = node_of(ir::NodeKind::Component, "node::2", "502", span(20, 460, 4, 5, 20));
    form.component = "comp::Form";
    std::vector<ir::ExprPtr> initial;
    initial.push_back(text("email="));
    initial.push_back(text("age=0"));
    form.props.emplace("initial", ir::make_array(std::move(initial)));
    form.events.emplace("validate", "bind::on_validate_form");
    form.events.emplace("onSubmit", "bind::on_submit");

    ir::Node email = node_of(ir::NodeKind::Component, "node::3", "503", span(30, 120, 5, 5, 80));
    email.component = "comp::Field";
    email.props.emplace("name", text("email"));
    email.props.emplace("label", text("Email"));
    email.props.emplace("required", ir::make_literal(true));
    std::vector<ir::ExprPtr> validators;
    validators.push_back(text("required"));
    validators.push_back(text("minLength:3"));
    email.props.emplace("validators", ir::make_array(std::move(validators)));
    email.children.push_back(text_input("node::4", "504", span(40, 20, 6, 7, 20)));

    ir::Node age = node_of(ir::NodeKind::Component, "node::5", "505", span(160, 120, 8, 5, 80));
    age.component = "comp::Field";
    age.props.emplace("name", text("age"));
    age.props.emplace("label", text("Age"));
    age.children.push_back(text_input("node::6", "506", span(170, 20, 9, 7, 20)));

    form.children.push_back(std::move(email));
    form.children.push_back(std::move(age));
    page.root.children.push_back(std::move(form));
    return page;
}

inline ir::IrModule make_forms_module() {
    ir::IrModule module;
    module.name = "demo";

    // The real library declarations: the same IR the compiler injects for a `"tessera"` import.
    const lowering::LibraryComponent* field = lowering::find_library_component("Field");
    const lowering::LibraryComponent* form = lowering::find_library_component("Form");
    if (field != nullptr) {
        module.components.push_back(lowering::make_library_component(*field));
    }
    if (form != nullptr) {
        module.components.push_back(lowering::make_library_component(*form));
    }
    module.components.push_back(make_field_page());
    module.components.push_back(make_form_page());

    ir::Binding validate = event_binding("bind::on_validate", "comp::FieldPage", "on_validate",
                                         span(60, 12, 5, 60, 72));
    validate.params = {{"e", "TextChangeEvent"}};
    module.bindings.push_back(std::move(validate));
    module.bindings.push_back(event_binding("bind::on_validate_form", "comp::FormPage",
                                            "on_validate_form", span(60, 12, 5, 60, 72)));
    module.bindings.push_back(
        event_binding("bind::on_submit", "comp::FormPage", "on_submit", span(70, 12, 6, 60, 72)));
    // IR order: bindings are sorted by id.
    std::sort(module.bindings.begin(), module.bindings.end(),
              [](const ir::Binding& left, const ir::Binding& right) { return left.id < right.id; });
    return module;
}

} // namespace forms_codegen_fixture
