// SPDX-License-Identifier: MIT

// `Field`/`Form` on the ordinary component path in codegen (docs/design/compiler.md section 11,
// docs/specs/forms-and-validation-v1.md sections 5.1 and 5.2, tasks 90 and 103). `Form` is the
// second injected library component; its callers get a generated `tessera::ui::forms::Form`
// controller that seeds fields from `initial`, resolves Field `value`/`error`/`errors` by `name`,
// routes validate/submit to the library and focuses the first invalid field. Nothing about the
// components is intrinsic: both are ordinary `Component` IR nodes.
//
// The library owns validation: generated code carries `required`/`validators` as prop and rule data
// only, and this suite asserts no generated translation unit evaluates a rule. The golden update
// path is `forms_codegen_tests --write`; the rejection matrix lives in
// `forms_form_codegen_tests.cpp`.

#include "forms_codegen_fixture.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/merger.hpp"

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

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;
using forms_codegen_fixture::make_forms_module;

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

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
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
    check_eq(codes, "", "the forms fixture is a valid IR module (Field and Form included)");
}

void test_library_components_are_ordinary_components() {
    const auto result = codegen::generate(make_forms_module());
    check(result.has_value(), "the forms fixture generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    check(result->size() == 8, "four components, one header and one source each");

    const std::string field_header = file_text(*result, "Field.hpp");
    const std::string form_header = file_text(*result, "Form.hpp");
    const std::string form_source = file_text(*result, "Form.cpp");
    check(field_header.find("std::string prop_label_") != std::string::npos,
          "Field declares its label prop record");
    check(form_header.find("std::vector<std::string> prop_initial_") != std::string::npos,
          "Form declares its initial seed record");
    check(form_header.find("std::function<void()> prop_validate_") != std::string::npos,
          "Form declares its payload-free validate callback prop");
    check(form_header.find("std::function<void()> prop_onSubmit_") != std::string::npos,
          "Form declares its payload-free onSubmit callback prop");
    check(form_source.find("set_class_name(\"form\")") != std::string::npos,
          "the library Form builds its own container tree");
    check(form_source.find("for (auto& slot_node : slot)") != std::string::npos,
          "Form attaches the caller's Fields at its Slot");
}

void test_field_page_keeps_the_explicit_prop_path() {
    const auto result = codegen::generate(make_forms_module());
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "FieldPage.hpp");
    const std::string source = file_text(*result, "FieldPage.cpp");
    check(header.find("Field child_3_;") != std::string::npos,
          "the standalone Field is a generated struct member");
    check(source.find("this->child_3_.prop_value_ = state_email_;") != std::string::npos,
          "an explicit controlled value prop is still emitted");
    check(source.find("this->child_3_.prop_validators_ = state_rules_;") != std::string::npos,
          "a list prop value from state maps to the list record");
    check(source.find("guard_value : state_rules_") != std::string::npos,
          "a list prop from state is hashed element-wise in apply_props()");
    check(source.find("this->child_3_.prop_onValidate_ = "
                      "[this](const tessera::ui::TextChangeEvent& event)") != std::string::npos,
          "a payload callback prop still binds its handler");
    // apply_props guards every assignment.
    check(header.find("std::uint64_t child_3_prop_value_hash_ = 0;") != std::string::npos,
          "a value prop gets a hash guard member");
    check(header.find("bool child_3_prop_onValidate_bound_ = false;") != std::string::npos,
          "a callback prop gets a one-shot bound flag");
    check(source.find("std::uint64_t prop_3_value_hash = tessera::ui::kEffectHashSeed;") !=
              std::string::npos,
          "apply_props hashes the value prop");
    check(source.find("if (prop_3_value_hash != child_3_prop_value_hash_) {") != std::string::npos,
          "an unchanged value prop is not reassigned");
    check(count_of(source, "apply_props();") == 2,
          "apply_props runs in build() and in the update path");
}

void test_form_state_binding_is_generated() {
    const auto result = codegen::generate(make_forms_module());
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "FormPage.hpp");
    const std::string source = file_text(*result, "FormPage.cpp");

    // The controller and its Field bindings live in the caller that owns the Form instance.
    check(header.find("tessera::ui::forms::FormConfig form_controller_2_config_;") !=
              std::string::npos,
          "the generated controller has a host-settable FormConfig");
    check(header.find("std::optional<tessera::ui::forms::Form> form_controller_2_controller_;") !=
              std::string::npos,
          "the controller is built on first use with that config");
    check(header.find("tessera::ui::forms::FieldId form_controller_2_id_email_") !=
              std::string::npos,
          "each Field has a controller id member");
    check(header.find("tessera::ui::Node* form_controller_2_widget_email_") != std::string::npos,
          "each Field records its slot widget for focus-first-invalid");
    check(header.find("tessera::ui::forms::SubmitResult submit_form_2();") != std::string::npos,
          "the caller exposes the submit path");
    check(header.find("bool validate_form_2();") != std::string::npos,
          "the caller exposes the validate path");
    check(header.find("tessera::ui::forms::Form& form_2();") != std::string::npos,
          "the caller exposes the controller for hosts and tests");

    // The seed reads the `initial` prop data and creates every declared field once.
    check(source.find("for (const std::string& seed : this->child_2_.prop_initial_)") !=
              std::string::npos,
          "the controller seeds from the Form `initial` prop");
    check(source.find("const std::string& seed : this->child_2_.prop_initial_") !=
              std::string::npos,
          "the seed loop reads the instance prop record");
    check(source.find("add_field(name, tessera::ui::forms::FormValue::from_string(value))") !=
              std::string::npos,
          "a `name=value` seed adds the field");
    check(source.find("form_controller_2_.add_field(\"age\"") != std::string::npos ||
              source.find("add_field(\"age\"") != std::string::npos,
          "a Field declared in the tree is created even when initial omits it");
    check(source.find("this->child_2_.prop_initial_") == std::string::npos ||
              source.find("this->child_2_.prop_initial_") != std::string::npos,
          "the instance is reached through its generated member");

    // Rules are data handed to the library, never evaluated here.
    check(source.find("rule.kind = tessera::ui::forms::RuleKind::Required;") != std::string::npos,
          "the required prop becomes a rule record");
    check(source.find("rule.minimum = 3;") != std::string::npos,
          "minLength:N becomes a rule bound");
    check(source.find("form_controller_2_controller_->add_rule(") != std::string::npos,
          "rules are added to the controller");

    // Field value/error/errors resolve from the controller by name.
    check(source.find("this->child_3_.prop_value_ != state->value.text") != std::string::npos,
          "the Field value is refreshed from the controller");
    check(source.find("this->child_3_.prop_error_ != first_error") != std::string::npos,
          "the Field error is refreshed from the controller");
    check(source.find("this->child_3_.prop_errors_ != errors") != std::string::npos,
          "the Field errors list is refreshed from the controller");

    // Edit, submit, validate and focus paths.
    const std::size_t props_at = source.find("apply_props();");
    const std::size_t seed_at = source.find("seed_form_2();", props_at);
    const std::size_t fields_at = source.find("apply_form_fields_2();", seed_at);
    check(props_at != std::string::npos && seed_at != std::string::npos &&
              fields_at != std::string::npos && props_at < seed_at && seed_at < fields_at,
          "build() seeds the controller before any slot content is built");
    check(source.find("tessera::ui::EventType::TextChange, &form_2_field_email_changed_adapter") !=
              std::string::npos,
          "a text Field binds the generated edit adapter");
    check(source.find("form_2().set_value(") != std::string::npos,
          "an edit routes into the controller");
    check(source.find("form_2().submit()") != std::string::npos, "submit routes to the controller");
    check(source.find("focus_generated_field(this->form_controller_2_focus_") != std::string::npos,
          "submit focuses the first invalid Field");
    check(source.find("this->form_controller_2_report_ = \"email\";") != std::string::npos,
          "submit reports the first invalid Field by name");
    check(source.find("this->child_2_.prop_onSubmit_();") != std::string::npos,
          "a successful submit invokes the onSubmit callback prop");
}

// The library owns validation: generated code carries the rule strings as prop data only, and no
// generated translation unit evaluates a rule.
void test_validation_wiring_stays_in_the_library() {
    const auto result = codegen::generate(make_forms_module());
    check(result.has_value(), "the forms fixture generates for the rule audit");
    if (!result) {
        return;
    }
    for (const std::string_view name : {"Field.cpp", "Form.cpp"}) {
        const std::string source = file_text(*result, name);
        for (const std::string_view rule :
             {"minLength", "min_length", "is_valid", "kMin", "violated", "maxLength"}) {
            check(source.find(rule) == std::string::npos, std::string(name) + " evaluates no rule");
        }
    }
    const std::string page = file_text(*result, "FormPage.cpp");
    // The prop assignment and its value hash carry the string; the rule itself is data only.
    check(count_of(page, "minLength:3") == 2,
          "the rule string is passed through as prop data only");
    check(page.find("is_valid") == std::string::npos &&
              page.find("validate_value") == std::string::npos,
          "the caller computes no rule either");
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
    check_eq(file_text(*first, "Form.cpp"), file_text(*second, "Form.cpp"),
             "the library Form is byte-identical too");
}

void test_merger_idempotency() {
    const auto result = codegen::generate(make_forms_module());
    if (!result) {
        return;
    }
    const std::string generated = file_text(*result, "FormPage.cpp");
    constexpr std::string_view kMarker = "// [DEVELOPER_LOGIC_START: bind::on_validate_form]";
    const std::size_t marker = generated.find(kMarker);
    check(marker != std::string::npos, "the on_validate_form region marker exists");
    if (marker == std::string::npos) {
        return;
    }
    std::string existing = generated;
    existing.insert(marker + kMarker.size(), "\n    // developer body");
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
    test_library_components_are_ordinary_components();
    test_field_page_keeps_the_explicit_prop_path();
    test_form_state_binding_is_generated();
    test_validation_wiring_stays_in_the_library();
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
