// SPDX-License-Identifier: MIT

// `Field` as a library component on the ordinary component path
// (docs/specs/forms-and-validation-v1.md section 5, docs/specs/component-model-v1.md section 3,
// task-90). A framework import names the built-in `Field` component, lowering emits an ordinary
// `Component` node, and the module carries the component's declared prop records: there is no
// intrinsic element and no Field-specific branch.
//
// The lowering-level checks here cover resolution, the prop-record mapping, the static Children
// slot, and determinism. Codegen diagnostics (unknown props, callback binding) and the generated
// C++ are covered by `forms_codegen_tests`; the runtime form is driven by
// `forms_codegen_runtime_tests`.

#include "lowering_module_test_util.hpp"
#include "tessera/schema/intrinsics.hpp"

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace lowering_module_test;

constexpr std::string_view kFormPage =
    "import { Field, TextInput, useState } from \"tessera\";\n"
    "function FormPage() {\n"
    "  const [email, setEmail] = useState<string>(\"\");\n"
    "  // Note: bind::on_validate\n"
    "  const validate = (e: TextChangeEvent): void => { setEmail(e.value); };\n"
    "  return (\n"
    "    <View className=\"form\">\n"
    "      <Field name=\"email\" label=\"Email\" value={email} defaultValue=\"\" required\n"
    "        validators={[\"required\", \"minLength:3\"]} onValidate={validate}\n"
    "        error=\"\" errors={[]}>\n"
    "        <TextInput inputMode=\"email\" />\n"
    "      </Field>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

const ir::Component* component_of(const lowering::LoweredModule& module, std::string_view name) {
    for (const ir::Component& component : module.components) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}

const ir::Prop* declared_prop(const ir::Component& component, std::string_view name) {
    for (const ir::Prop& prop : component.props) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

const ir::Expr* prop_value(const ir::Node& node, std::string_view name) {
    const auto found = node.props.find(std::string(name));
    return found == node.props.end() ? nullptr : found->second.get();
}

bool is_string_literal(const ir::Expr* expr, std::string_view expected) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return false;
    }
    const auto* text = std::get_if<std::string>(&expr->literal);
    return text != nullptr && *text == expected;
}

bool is_bool_literal(const ir::Expr* expr, bool expected) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return false;
    }
    const auto* flag = std::get_if<bool>(&expr->literal);
    return flag != nullptr && *flag == expected;
}

bool is_ref(const ir::Expr* expr, std::string_view expected) {
    return expr != nullptr && expr->kind == ir::ExprKind::Ref && expr->id == expected;
}

bool is_string_list(const ir::Expr* expr, std::span<const std::string_view> expected) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Array ||
        expr->operands.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!is_string_literal(expr->operands[i].get(), expected[i])) {
            return false;
        }
    }
    return true;
}

void test_field_resolves_to_a_component_node() {
    const auto fixture = parse_file("ui/FormPage.tsx", std::string(kFormPage));
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(), "a Field usage lowers");
    if (!result) {
        std::cerr << "  error: " << first_message(result) << "\n";
        return;
    }
    const ir::Component* page = component_of(*result, "FormPage");
    check(page != nullptr, "the page component is present");
    if (page == nullptr) {
        return;
    }
    // The framework import injects the library component's IR declaration into the module.
    const ir::Component* field = component_of(*result, "Field");
    check(field != nullptr, "the library Field component is part of the module");
    if (field != nullptr) {
        check_eq(field->id, "comp::Field", "the library component id");
    }
    // No intrinsic element is involved: `<Field>` is a component reference.
    check(tessera::schema::find_element("Field") == nullptr,
          "Field is not an intrinsic element of the schema table");

    check(page->root.children.size() == 1, "the page root has the Field instance");
    if (page->root.children.size() != 1) {
        return;
    }
    const ir::Node& instance = page->root.children[0];
    check_eq(std::string(ir::node_kind_name(instance.kind)), "Component",
             "the Field usage is a Component IR node, not an intrinsic");
    check_eq(instance.component, "comp::Field", "the node targets the library component");
    check(instance.kind != ir::NodeKind::View && instance.kind != ir::NodeKind::Text,
          "the Field usage is not lowered as an intrinsic node kind");
}

void test_prop_records_are_mapped() {
    const auto fixture = parse_file("ui/FormPage.tsx", std::string(kFormPage));
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(), "the Field fixture lowers for the prop matrix");
    if (!result) {
        return;
    }
    const ir::Component* page = component_of(*result, "FormPage");
    const ir::Component* field = component_of(*result, "Field");
    if (page == nullptr || field == nullptr || page->root.children.empty()) {
        check(false, "the fixture has the page and the library component");
        return;
    }
    const ir::Node& instance = page->root.children[0];

    // The documented field prop records (forms section 5.1) and the value each attribute lowers to.
    check(is_string_literal(prop_value(instance, "name"), "email"),
          "`name` maps to the string prop record");
    check(is_string_literal(prop_value(instance, "label"), "Email"),
          "`label` maps to the string prop record");
    check(is_ref(prop_value(instance, "value"), "state::email"),
          "`value` maps to the controlled value expression");
    check(is_string_literal(prop_value(instance, "defaultValue"), ""),
          "`defaultValue` maps to the string prop record");
    check(is_bool_literal(prop_value(instance, "required"), true),
          "`required` maps to the bool prop record");
    const std::string_view validators[] = {"required", "minLength:3"};
    check(is_string_list(prop_value(instance, "validators"), validators),
          "`validators` maps to the list<string> prop record");
    check(is_string_literal(prop_value(instance, "error"), ""),
          "`error` maps to the string prop record");
    check(is_string_list(prop_value(instance, "errors"), {}),
          "`errors` maps to the list<string> prop record");
    check(prop_value(instance, "onValidate") == nullptr, "a callback prop is not a value prop");
    const auto callback = instance.events.find("onValidate");
    check(callback != instance.events.end() && callback->second == "bind::on_validate",
          "`onValidate` is a callback prop bound through the node's events");

    // The declared records the module carries for the library component.
    struct Expected {
        const char* name;
        const char* type;
    };
    const Expected expected[] = {{"name", "string"},
                                 {"label", "string"},
                                 {"value", "string"},
                                 {"defaultValue", "string"},
                                 {"required", "bool"},
                                 {"validators", "list<string>"},
                                 {"onValidate", "TextChangeEvent"},
                                 {"error", "string"},
                                 {"errors", "list<string>"},
                                 {"children", "children"}};
    for (const Expected& record : expected) {
        const ir::Prop* prop = declared_prop(*field, record.name);
        check(prop != nullptr, std::string("Field declares `") + record.name + "`");
        if (prop != nullptr) {
            check_eq(prop->type, record.type,
                     std::string("Field prop `") + record.name + "` has its documented type");
        }
    }
}

void test_children_ride_the_static_slot() {
    const auto fixture = parse_file("ui/FormPage.tsx", std::string(kFormPage));
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(), "the Field fixture lowers for the slot check");
    if (!result) {
        return;
    }
    const ir::Component* page = component_of(*result, "FormPage");
    const ir::Component* field = component_of(*result, "Field");
    if (page == nullptr || field == nullptr || page->root.children.empty()) {
        check(false, "the fixture has the page and the library component");
        return;
    }
    const ir::Node& instance = page->root.children[0];
    check(instance.children.size() == 1, "the widget is the instance's slot content");
    if (instance.children.size() == 1) {
        check_eq(std::string(ir::node_kind_name(instance.children[0].kind)), "TextInput",
                 "the slot content is the caller's TextInput");
    }
    const ir::Prop* children = declared_prop(*field, "children");
    check(children != nullptr && children->type == "children",
          "Field declares the static children slot");
    bool places_slot = false;
    for (const ir::Node& child : field->root.children) {
        places_slot = places_slot || child.kind == ir::NodeKind::Slot;
    }
    check(places_slot, "the library Field places its Slot in its own tree");
}

void test_lowering_is_deterministic() {
    const auto first_fixture = parse_file("ui/FormPage.tsx", std::string(kFormPage));
    const auto first = lowering::lower_module(first_fixture->module_source(), "demo");
    const auto second_fixture = parse_file("ui/FormPage.tsx", std::string(kFormPage));
    const auto second = lowering::lower_module(second_fixture->module_source(), "demo");
    check(first.has_value() && second.has_value(), "the Field fixture lowers twice");
    if (!first || !second) {
        return;
    }
    check_eq(describe(*first), describe(*second),
             "two lowers of the same Field usage are byte-identical");
}

void test_a_project_component_wins_over_the_library_one() {
    const auto fixture =
        parse_file("ui/App.tsx", "import { Field } from \"tessera\";\n"
                                 "function Field() { return <View className=\"own\" />; }\n"
                                 "function App() { return <Field />; }\n");
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(), "a project Field declaration is allowed next to the import");
    if (!result) {
        return;
    }
    std::size_t fields = 0;
    for (const ir::Component& component : result->components) {
        if (component.name == "Field") {
            ++fields;
        }
    }
    check(fields == 1, "the project declaration is not duplicated by the library one");
    const ir::Component* page = component_of(*result, "App");
    if (page != nullptr && !page->root.children.empty()) {
        check_eq(std::string(ir::node_kind_name(page->root.children[0].kind)), "View",
                 "the local declaration is still the reference target");
    }
}

} // namespace

int main() {
    test_field_resolves_to_a_component_node();
    test_prop_records_are_mapped();
    test_children_ride_the_static_slot();
    test_lowering_is_deterministic();
    test_a_project_component_wins_over_the_library_one();
    if (jsx_test::failures != 0) {
        std::cerr << jsx_test::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "forms lowering tests passed\n";
    return 0;
}
