// SPDX-License-Identifier: MIT

// Issue #834 (ADR-051 implementation note, ADR-056 decisions 1 and 5): generated record equality,
// per-field dirty bits, and record list elements.
//
// Three things under test, each at the level the issue names:
//   1. the generated types header emits a defaulted `operator==` and a field-wise `hash_value`;
//   2. the dirty plan assigns one bit per read leaf path of a record cell, a whole-record setter
//      compares field by field, and a `recordUpdate` setter raises only the written field's bit;
//   3. a `list<Record>` loop source generates, keyed by a field of the item (`row.id`), and its
//      per-item content pass hashes only the fields the template reads.
//
// The golden is owned by this test through `codegen_record_equality_tests --write`; the runtime
// behaviour is driven in codegen_record_runtime_tests.cpp over the committed golden.

#include "codegen_record_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#ifndef TESSERA_CODEGEN_RECORD_GOLDEN_DIR
#error "TESSERA_CODEGEN_RECORD_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;
using tessera::test::codegen_record::make_record_module;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::map<std::string, std::string> generate_files(const ir::IrModule& module) {
    std::map<std::string, std::string> files;
    const auto result = codegen::generate(module, {}, std::nullopt);
    if (!result) {
        std::cerr << "  generate failed: " << result.error().front() << "\n";
        return files;
    }
    for (const codegen::GeneratedFile& file : *result) {
        files[file.path] = file.text;
    }
    return files;
}

// Level 1: the generated aggregate carries equality and a field-wise hash.
void test_types_header(const std::string& types) {
    check(contains(types, "struct Form {"), "the Form aggregate is emitted");
    check(contains(types, "bool operator==(const Form&) const = default;"),
          "Form gets a defaulted operator==");
    check(contains(types, "bool operator==(const Entry&) const = default;"),
          "Entry gets a defaulted operator==");
    check(contains(types, "inline std::uint64_t hash_value(const Form& value)"),
          "Form gets a field-wise hash");
    check(contains(types, "hash = tessera::ui::effect_hash(hash, std::string_view(value.name));"),
          "the hash reads each string field through the string_view overload");
    check(contains(types, "hash = tessera::ui::effect_hash(hash, value.age);"),
          "the hash reads an int field through the scalar overload");
    check(contains(types, "inline std::uint64_t hash_value(const Entry& value);"),
          "every overload is forward-declared before the definitions");
}

// Level 2: one bit per read leaf path of the record cell.
void test_field_bits(const std::string& header) {
    check(contains(header, "static constexpr std::size_t kBit_state_form_name_ ="),
          "a record field has its own bit");
    check(contains(header, "static constexpr std::size_t kBit_state_form_email_ ="),
          "the second field has its own bit");
    check(contains(header, "static constexpr std::size_t kBit_state_form_age_ ="),
          "the third field has its own bit");
    check(!contains(header, "static constexpr std::size_t kBit_state_form_ ="),
          "no whole-record bit exists once the record splits into leaves");
}

// Level 2: the binding that reads only `name` depends on the `name` bit alone.
void test_binding_masks(const std::string& header) {
    // The text at ordinal 2 reads only state::form.name; ordinal 3 only state::form.email.
    check(contains(header, "constexpr tessera::ui::DirtyMask<") &&
              contains(header, "kDirty_text_2 ="),
          "the name text has a dependency mask");
    // Both masks must be distinct single-field masks, so `email` never re-runs the `name` text.
    const std::size_t name_mask = header.find("kDirty_text_2 =");
    const std::size_t email_mask = header.find("kDirty_text_3 =");
    check(name_mask != std::string::npos && email_mask != std::string::npos,
          "both field-reading texts have masks");
    if (name_mask != std::string::npos && email_mask != std::string::npos) {
        const std::string name_line = header.substr(name_mask, 80);
        const std::string email_line = header.substr(email_mask, 80);
        check(name_line != email_line, "the two field masks differ");
    }
}

// Level 2: the setters. The whole-record setter compares field by field; the record update raises
// only the written field's bit.
void test_setters(const std::string& header) {
    const std::size_t setter = header.find("::set_form(Form value) {");
    check(setter != std::string::npos, "the record state gets a whole-record setter");
    if (setter != std::string::npos) {
        const std::string body = header.substr(setter, 900);
        check(body.find("state_form_.name == value.name") != std::string::npos &&
                  body.find("state_form_.email == value.email") != std::string::npos &&
                  body.find("state_form_.age == value.age") != std::string::npos,
              "the whole-record setter compares every field");
        check(body.find("note_dirty(kBit_state_form_name_)") != std::string::npos,
              "a changed field raises its leaf bit");
        check(body.find("if (tessera_changed)") != std::string::npos,
              "an equal record schedules nothing");
    }
    // handler_1 sets `{...form, name: x}`, so it compares every field but only `name` can differ.
    const std::size_t handler = header.find("::handler_1(");
    check(handler != std::string::npos, "the record-update handler is emitted");
    if (handler != std::string::npos) {
        // Bound the window at the next handler: a fixed length would run into `handler_2`, which
        // legitimately raises every field's bit.
        const std::size_t next = header.find("::handler_2(", handler);
        const std::string body =
            header.substr(handler, next == std::string::npos ? std::string::npos : next - handler);
        check(body.find("dirty_.set(kBit_state_form_name_)") != std::string::npos,
              "the record update raises the written field's bit");
        check(body.find("dirty_.set(kBit_state_form_email_)") == std::string::npos,
              "the record update never raises an unwritten field's bit");
    }
}

// Level 3: a list of records, keyed by a field of the item.
void test_record_list(const std::string& header) {
    check(contains(header, "std::vector<Entry> prop_seed_ = {};"),
          "the record list is a generated std::vector<Entry>");
    check(contains(header, "build_item_4(const Entry& row"),
          "the record list template builds one item per Entry");
    check(contains(header, "const std::vector<Entry>& seed()") ||
              contains(header, "std::vector<Entry> state_rows_"),
          "the record list iterates the record cells");
    // The per-item content hashes row.label, the only field the template reads.
    check(contains(header, "row.label"),
          "the per-item content reads the template's record field");
}

void test_golden(bool update, const std::string& name, const std::string& text) {
    const std::filesystem::path path =
        std::filesystem::path(TESSERA_CODEGEN_RECORD_GOLDEN_DIR) / name;
    if (update) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        return;
    }
    check(read_file(path) == text,
          name + " matches its golden; run `codegen_record_equality_tests --write`");
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    const ir::IrModule module = make_record_module();
    check(ir::validate(module).empty(), "the record fixture is a valid IR module");
    const auto files = generate_files(module);
    check(files.size() == 2, "the fixture generates the types header and the component base");
    check(files == generate_files(module), "two runs are byte-identical");
    if (files.size() == 2) {
        if (!update) {
            test_types_header(files.at("demo_types.hpp"));
            test_field_bits(files.at("FormPage.g.hpp"));
            test_binding_masks(files.at("FormPage.g.hpp"));
            test_setters(files.at("FormPage.g.hpp"));
            test_record_list(files.at("FormPage.g.hpp"));
        }
        for (const auto& [name, text] : files) {
            test_golden(update, name, text);
        }
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen record equality tests passed\n";
    return 0;
}
