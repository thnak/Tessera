// SPDX-License-Identifier: MIT

// `Field` on the ordinary component path in codegen (docs/design/compiler.md section 11,
// docs/specs/forms-and-validation-v1.md section 5, task-90). The module carries the real built-in
// `Field` IR declaration plus a form component with two Field instances. Codegen passes the value
// props through the declared prop records of the instance, binds the record-typed `onValidate`
// callback through the node's events, and puts the caller's widget in the static Children slot.
// Nothing about `Field` is intrinsics: the element is a `Component` node and the generated host
// node records the `Component` element kind.
//
// The validation rules are not here by construction: `Field` carries `required`/`validators` as
// prop data and this suite asserts the generated code contains no rule evaluation. The golden
// update path is `forms_codegen_tests --write`.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/lowering/library_components.hpp"
#include "tessera/compiler/merger.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace ir = tessera::compiler::ir;
namespace codegen = tessera::compiler::codegen;
namespace lowering = tessera::compiler::lowering;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(const std::string& actual, const std::string& expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
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

ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id, ir::SourceSpan source) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = std::move(source);
    return node;
}

ir::IrModule make_forms_module() {
    ir::IrModule module;
    module.name = "demo";

    // The real library declaration: the same IR the compiler injects for `import { Field }`.
    const lowering::LibraryComponent* field = lowering::find_library_component("Field");
    if (field == nullptr) {
        check(false, "the Field library component is declared");
        return module;
    }
    module.components.push_back(lowering::make_library_component(*field));

    ir::Component page;
    page.id = "comp::FormPage";
    page.component_id = "71";
    page.name = "FormPage";
    page.source = span(0, 500, 1, 1, 24);
    page.state.push_back({"state::email", "email", "string", text("")});

    page.root = node_of(ir::NodeKind::View, "node::1", "501", span(10, 400, 3, 3, 30));
    page.root.props.emplace("className", text("form"));

    ir::Node button = node_of(ir::NodeKind::Button, "node::2", "502", span(20, 20, 4, 5, 20));
    button.props.emplace("label", text("Set"));
    button.events.emplace("onClick", "handler::1");

    // First Field: a required, min-length email field wrapping the caller's TextInput.
    ir::Node email_field =
        node_of(ir::NodeKind::Component, "node::3", "503", span(30, 80, 5, 5, 80));
    email_field.component = "comp::Field";
    email_field.props.emplace("name", text("email"));
    email_field.props.emplace("label", text("Email"));
    email_field.props.emplace("value", ir::make_ref("state::email"));
    email_field.props.emplace("required", ir::make_literal(true));
    std::vector<ir::ExprPtr> validators;
    validators.push_back(text("required"));
    validators.push_back(text("minLength:3"));
    email_field.props.emplace("validators", ir::make_array(std::move(validators)));
    email_field.props.emplace("error", text(""));
    email_field.events.emplace("onValidate", "bind::on_validate");
    email_field.children.push_back(
        node_of(ir::NodeKind::TextInput, "node::4", "504", span(40, 20, 6, 7, 20)));

    // Second Field: a static age field with its own slot widget.
    ir::Node age_field =
        node_of(ir::NodeKind::Component, "node::5", "505", span(120, 60, 8, 5, 60));
    age_field.component = "comp::Field";
    age_field.props.emplace("name", text("age"));
    age_field.props.emplace("label", text("Age"));
    age_field.props.emplace("value", text("0"));
    age_field.props.emplace("defaultValue", text("0"));
    age_field.children.push_back(
        node_of(ir::NodeKind::TextInput, "node::6", "506", span(130, 20, 9, 7, 20)));

    page.root.children.push_back(std::move(button));
    page.root.children.push_back(std::move(email_field));
    page.root.children.push_back(std::move(age_field));

    ir::Handler set_email;
    set_email.id = "handler::1";
    set_email.source = span(15, 18, 3, 8, 26);
    ir::Action set;
    set.kind = ir::ActionKind::Set;
    set.target = "state::email";
    set.value = text("ada@example.com");
    set_email.actions.push_back(std::move(set));
    page.handlers.push_back(std::move(set_email));
    module.components.push_back(std::move(page));

    ir::Binding validate;
    validate.id = "bind::on_validate";
    validate.component = "comp::FormPage";
    validate.kind = ir::BindingKind::Event;
    validate.region = "on_validate";
    validate.params = {{"e", "TextChangeEvent"}};
    validate.source = span(60, 12, 5, 60, 72);
    module.bindings.push_back(std::move(validate));
    return module;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text_stream;
    text_stream << in.rdbuf();
    return text_stream.str();
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

std::string file_text(const std::vector<codegen::GeneratedFile>& files, std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

std::size_t count_of(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

void test_fixture_is_valid() {
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(make_forms_module())) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the forms fixture is a valid IR module (Field included)");
}

void test_generated_shape() {
    const auto result = codegen::generate(make_forms_module());
    check(result.has_value(), "the forms fixture generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    check(result->size() == 4, "two components, one header and one source each");

    // The library Field is generated like any other component: declared prop members, its own tree,
    // and the static slot it places.
    const std::string field_header = file_text(*result, "Field.hpp");
    const std::string field_source = file_text(*result, "Field.cpp");
    check(field_header.find("std::string prop_label_") != std::string::npos,
          "Field declares its label prop record");
    check(field_header.find("std::vector<std::string> prop_validators_") != std::string::npos,
          "Field declares its validators prop record (library data, not a rule)");
    check(field_header.find("bool prop_required_") != std::string::npos,
          "Field declares its required prop record");
    check(field_header.find("std::function<void(const tessera::ui::TextChangeEvent&)> "
                            "prop_onValidate_") != std::string::npos,
          "Field declares its callback prop record");
    check(field_source.find("void Field::build(tessera::ui::Node& parent, "
                            "std::vector<std::unique_ptr<tessera::ui::Node>> slot)") !=
              std::string::npos,
          "Field takes the caller's widget through the static Children slot");
    check(field_source.find("for (auto& slot_node : slot)") != std::string::npos,
          "Field attaches the caller's widget at its Slot");
    check(field_source.find("set_class_name(\"field\")") != std::string::npos,
          "Field builds its own library tree");

    // The caller instantiates the two Fields through the ordinary component path.
    const std::string page_header = file_text(*result, "FormPage.hpp");
    const std::string page_source = file_text(*result, "FormPage.cpp");
    check(page_header.find("Field child_3_;") != std::string::npos &&
              page_header.find("Field child_5_;") != std::string::npos,
          "each Field instance is a generated struct member");
    check(page_header.find("void apply_props();") != std::string::npos,
          "the caller declares the generated prop refresh");
    check(count_of(page_source, "set_element_kind(\"Component\")") == 2,
          "both Field instances get an ordinary Component host node");
    check(page_source.find("child_3_.build(*node_3, std::move(slot_3));") != std::string::npos,
          "the Field instance is built with the caller's slot content");
    check(page_source.find("this->child_3_.prop_name_ = \"email\";") != std::string::npos,
          "a value prop maps to the declared prop record");
    check(page_source.find("this->child_3_.prop_value_ = state_email_;") != std::string::npos,
          "a controlled value prop keeps its expression");
    check(page_source.find("this->child_3_.prop_required_ = true;") != std::string::npos,
          "a flag prop maps to the bool record");
    check(page_source.find("this->child_3_.prop_validators_ = {\"required\", \"minLength:3\"};") !=
              std::string::npos,
          "a list prop maps to the list<string> record");
    check(page_source.find("this->child_3_.prop_onValidate_ = "
                           "[this](const tessera::ui::TextChangeEvent& event)") !=
              std::string::npos,
          "the callback prop binds a generated callable");
    check(page_source.find("this->on_validate(event);") != std::string::npos,
          "the callback forwards to the caller's binding");
    check(count_of(page_source, "apply_props();") == 2,
          "apply_props runs in build() and in the update path");
}

// The library owns validation: the generated code carries the rule strings as prop data only, and
// no generated translation unit computes a rule.
void test_validation_wiring_stays_in_the_library() {
    const auto result = codegen::generate(make_forms_module());
    check(result.has_value(), "the forms fixture generates for the rule audit");
    if (!result) {
        return;
    }
    const std::string field_source = file_text(*result, "Field.cpp");
    const std::string page_source = file_text(*result, "FormPage.cpp");
    for (const std::string_view rule :
         {"minLength", "min_length", "is_valid", "kMin", "violated", "maxLength"}) {
        check(field_source.find(rule) == std::string::npos,
              "the generated library Field evaluates no rule");
    }
    // The caller only forwards the rule strings as prop data.
    check(count_of(page_source, "minLength:3") == 1,
          "the rule string is passed through once as a prop record");
    check(page_source.find("is_valid") == std::string::npos &&
              page_source.find("validate_value") == std::string::npos,
          "the caller computes no rule either");
}

void test_prop_rejections() {
    {
        ir::IrModule module = make_forms_module();
        module.components[1].root.children[1].props.erase("required");
        module.components[1].root.children[1].props.emplace("minLength", text(std::string("3")));
        const auto result = codegen::generate(module);
        check(!result.has_value(), "an unknown Field prop is rejected");
        check(first_error(result).find("has no prop `minLength`") != std::string::npos,
              "the unknown prop diagnostic names the prop and the component");
    }
    {
        ir::IrModule module = make_forms_module();
        module.components[1].root.children[1].props.emplace("onValidate", text(std::string("x")));
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a callback prop passed as a value is rejected");
        check(first_error(result).find("bound through the node's events") != std::string::npos,
              "the diagnostic explains the callback binding");
    }
    {
        ir::IrModule module = make_forms_module();
        module.components[1].root.children[1].events.emplace("label", "handler::1");
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a value prop bound as an event is rejected");
        check(first_error(result).find("is not a callback prop") != std::string::npos,
              "the diagnostic names the non-callback prop");
    }
    {
        ir::IrModule module = make_forms_module();
        module.components[1].root.children[1].props.at("required") = text(std::string("yes"));
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a literal of the wrong prop type is rejected");
        check(first_error(result).find("expects `bool`") != std::string::npos,
              "the diagnostic names the expected type");
    }
    {
        ir::IrModule module = make_forms_module();
        module.components[1].root.children[1].props.emplace("children", text(std::string("x")));
        const auto result = codegen::generate(module);
        check(!result.has_value(), "children cannot be passed as a value prop");
        check(first_error(result).find("is the slot") != std::string::npos,
              "the diagnostic points at the slot attribute");
    }
    {
        ir::IrModule module = make_forms_module();
        module.components[1].root.children[1].events.at("onValidate") = "handler::1";
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a callback with a mismatched handler signature is rejected");
        check(first_error(result).find("taking one `TextChangeEvent` parameter") !=
                  std::string::npos,
              "the diagnostic asks for the callback payload record");
    }
}

void test_goldens(bool update) {
    const auto result = codegen::generate(make_forms_module());
    check(result.has_value(), "the forms golden sample generates");
    if (!result) {
        return;
    }
    const std::string directory = TESSERA_CODEGEN_GOLDEN_DIR;
    if (update) {
        std::filesystem::create_directories(directory);
    }
    for (const codegen::GeneratedFile& file : *result) {
        const std::string path = directory + "/" + file.path;
        if (update) {
            write_file(path, file.text);
            continue;
        }
        const std::string golden = read_file(path);
        check(!golden.empty(), "golden " + file.path + " is readable");
        if (golden != file.text) {
            check(false, file.path + " differs from its golden; run `forms_codegen_tests --write`");
        }
    }
    if (update) {
        std::cout << "forms goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = codegen::generate(make_forms_module());
    const auto second = codegen::generate(make_forms_module());
    check(first.has_value() && second.has_value(), "the forms sample generates twice");
    if (!first || !second) {
        return;
    }
    check_eq(file_text(*first, "FormPage.cpp"), file_text(*second, "FormPage.cpp"),
             "two runs are byte-identical");
    check_eq(file_text(*first, "Field.cpp"), file_text(*second, "Field.cpp"),
             "the library Field is byte-identical too");
}

void test_merger_idempotency() {
    const auto result = codegen::generate(make_forms_module());
    check(result.has_value(), "the forms sample generates for the merger test");
    if (!result) {
        return;
    }
    const std::string generated = file_text(*result, "FormPage.cpp");
    constexpr std::string_view kMarker = "// [DEVELOPER_LOGIC_START: bind::on_validate]";
    const std::size_t marker = generated.find(kMarker);
    check(marker != std::string::npos, "the on_validate region marker exists");
    if (marker == std::string::npos) {
        return;
    }
    std::string existing = generated;
    existing.insert(marker + kMarker.size(), "\n    (void)e; // developer body");
    const tessera::compiler::MergeResult first = tessera::compiler::merge(generated, existing);
    check(first.text.find("developer body") != std::string::npos,
          "the developer body survives the merge");
    const tessera::compiler::MergeResult second = tessera::compiler::merge(first.text, existing);
    check_eq(second.text, first.text, "merge(merge(g, e), e) == merge(g, e) over the forms output");
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    if (update) {
        test_goldens(true);
        return failures == 0 ? 0 : 1;
    }
    test_fixture_is_valid();
    test_generated_shape();
    test_validation_wiring_stays_in_the_library();
    test_prop_rejections();
    test_goldens(false);
    test_determinism();
    test_merger_idempotency();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "forms codegen tests passed\n";
    return 0;
}
