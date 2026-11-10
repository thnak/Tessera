// SPDX-License-Identifier: MIT

// Rejection matrix of the generated form-state binding (docs/specs/forms-and-validation-v1.md
// section 5.2, task-103). The binding rejects what it cannot bind statically: a Field prop the
// controller owns, a missing/duplicate `name`, a seed that is not `name=value`, a dynamic or
// unsupported rule, a nested Form, and a callback whose handler has the wrong shape. Every
// diagnostic carries TESSERA_CODEGEN_UNSUPPORTED and names the Field/Form.

#include "forms_codegen_fixture.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;
using forms_codegen_fixture::make_forms_module;
using forms_codegen_fixture::node_of;
using forms_codegen_fixture::span;
using forms_codegen_fixture::text;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_rejected(const ir::IrModule& module, std::string_view expected, std::string_view what) {
    const auto result = codegen::generate(module);
    check(!result.has_value(), what);
    if (result) {
        return;
    }
    const std::string error = result.error().empty() ? std::string() : result.error().front();
    if (error.find(expected) == std::string::npos) {
        std::cerr << "FAIL: " << what << "\n  expected substring: " << expected
                  << "\n  actual: " << error << "\n";
        ++failures;
    }
}

std::size_t component_index(const ir::IrModule& module, std::string_view name) {
    for (std::size_t i = 0; i < module.components.size(); ++i) {
        if (module.components[i].name == name) {
            return i;
        }
    }
    return module.components.size();
}

ir::Node& form_page(ir::IrModule& module) {
    return module.components[component_index(module, "FormPage")].root.children[0];
}

ir::Node& field_node(ir::IrModule& module, std::size_t index) {
    return module.components[component_index(module, "FormPage")].root.children[0].children[index];
}

void test_the_fixture_generates() {
    const auto result = codegen::generate(make_forms_module());
    check(result.has_value(), "the unmodified fixture generates (rejection control)");
}

void test_field_props_the_controller_owns_are_rejected() {
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props.emplace("value", text("explicit"));
        check_rejected(module, "the Form controller resolves that prop",
                       "an explicit Field value inside a Form is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props.emplace("error", text("bad"));
        check_rejected(module, "the Form controller resolves that prop",
                       "an explicit Field error inside a Form is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).children[0].props.emplace("value", text("x"));
        check_rejected(module, "the Form controller owns the field value",
                       "the widget cannot own the form value");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).children[0].events.emplace("onChange", "handler::1");
        check_rejected(module, "binds `onChange`", "the widget cannot bind its own change event");
    }
}

void test_field_identity_is_checked() {
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props.erase("name");
        check_rejected(module, "needs a string-literal `name`",
                       "a Field without a name is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props["name"] = text("");
        check_rejected(module, "needs a string-literal `name`", "an empty Field name is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 1).props["name"] = text("email");
        check_rejected(module, "must be unique", "duplicate Field names are rejected");
    }
}

void test_seeds_are_validated() {
    {
        ir::IrModule module = make_forms_module();
        form_page(module).props["initial"] = ir::make_ref("state::initial");
        check_rejected(module, "must be a literal `name=value` string array",
                       "a dynamic initial is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        form_page(module).props["initial"] = forms_codegen_fixture::string_array({"email"});
        check_rejected(module, "is not `name=value`", "a seed without `=` is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        form_page(module).props["initial"] = forms_codegen_fixture::string_array({"="});
        check_rejected(module, "is not `name=value`", "a seed without a name is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        form_page(module).props["initial"] =
            forms_codegen_fixture::string_array({"email=", "email=x"});
        check_rejected(module, "seeds `email` more than once", "a duplicate seed is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        form_page(module).props["initial"] = ir::make_array([] {
            std::vector<ir::ExprPtr> items;
            items.push_back(ir::make_literal(true));
            return items;
        }());
        check_rejected(module, "expects `list<string>`", "a non-string seed is rejected");
    }
}

void test_rules_must_be_static_data() {
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props["required"] = ir::make_ref("state::email");
        check_rejected(module, "must be a literal bool",
                       "a dynamic required flag is deferred and rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props["validators"] = ir::make_ref("state::email");
        check_rejected(module, "must be a literal string array",
                       "dynamic validators are deferred and rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props["validators"] =
            forms_codegen_fixture::string_array({"maxDigits:2"});
        check_rejected(module, "is not a supported rule", "an unknown validator is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        field_node(module, 0).props["validators"] =
            forms_codegen_fixture::string_array({"minLength:many"});
        check_rejected(module, "needs a non-negative integer bound",
                       "a malformed validator bound is rejected");
    }
}

void test_form_and_callback_shape() {
    {
        ir::IrModule module = make_forms_module();
        ir::Node& form = form_page(module);
        ir::Node nested =
            node_of(ir::NodeKind::Component, "node::7", "507", span(200, 20, 10, 5, 20));
        nested.component = "comp::Form";
        form.children.push_back(std::move(nested));
        check_rejected(module, "inside another Form", "a nested Form is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        form_page(module).props.emplace("onSubmit", text("x"));
        check_rejected(module, "bound through the node's events",
                       "a callback prop passed as a value is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        ir::Binding payload;
        payload.id = "bind::on_submit_payload";
        payload.component = "comp::FormPage";
        payload.kind = ir::BindingKind::Event;
        payload.region = "on_submit_payload";
        payload.params = {{"e", "TextChangeEvent"}};
        payload.source = span(80, 12, 7, 60, 72);
        module.bindings.push_back(std::move(payload));
        form_page(module).events["onSubmit"] = "bind::on_submit_payload";
        check_rejected(module, "needs a handler taking no parameter",
                       "a payload callback on a payload-free prop is rejected");
    }
    {
        ir::IrModule module = make_forms_module();
        form_page(module).events["validate"] = "bind::on_validate";
        check_rejected(module, "names unknown binding",
                       "a callback naming another component's binding is rejected");
    }
}

} // namespace

int main() {
    test_the_fixture_generates();
    test_field_props_the_controller_owns_are_rejected();
    test_field_identity_is_checked();
    test_seeds_are_validated();
    test_rules_must_be_static_data();
    test_form_and_callback_shape();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "forms form codegen tests passed\n";
    return 0;
}
