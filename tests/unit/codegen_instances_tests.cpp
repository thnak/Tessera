// SPDX-License-Identifier: MIT

// Per-branch and per-item component instances in codegen (issue #231,
// docs/design/compiler-node-state.md): a conditional instance keeps its instance in the branch
// slot, a keyed list template owns one instance per item in the item record, and a conditional
// inside a template becomes per-item presence state. The fixture is hand-built IR
// (codegen_instances_fixture.hpp); the stored goldens are owned by `--write` and compiled by the
// build as `tessera_codegen_instances_sample`, which the runtime test drives.

#include "codegen_instances_fixture.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_CODEGEN_INSTANCES_GOLDEN_DIR
#error "TESSERA_CODEGEN_INSTANCES_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;

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

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_file(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string file_text(const std::vector<codegen::GeneratedFile>& files, std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

void test_fixture_is_valid() {
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic :
         ir::validate(instances_fixture::make_instances_module())) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check(codes.empty(), "the instance fixture is a valid IR module");
    if (!codes.empty()) {
        std::cerr << "  " << codes << "\n";
    }
}

void test_generated_shape() {
    const auto result = codegen::generate(instances_fixture::make_instances_module());
    check(result.has_value(), "the instance fixture generates");
    if (!result) {
        std::cerr << "  " << (result.error().empty() ? std::string() : result.error().front())
                  << "\n";
        return;
    }
    const std::string page_header = file_text(*result, "Page.hpp");
    const std::string page_source = file_text(*result, "Page.cpp");
    const std::string row_header = file_text(*result, "Row.hpp");
    check(!page_header.empty() && !page_source.empty() && !row_header.empty(),
          "both components generate a header and a source");

    // The keyed item record owns one instance per item (section 3).
    check(contains(page_header, "struct Item_4 {"), "the keyed template gets an item record");
    check(contains(page_header, "std::unique_ptr<Row> child_4;"),
          "the item record owns the per-item instance by pointer (contracts stay addressable)");
    check(contains(page_header, "std::uint64_t child_4_prop_label_hash_ = 0;"),
          "the item record owns the per-item prop guard");
    check(contains(page_header, "bool mounted_ = false;") &&
              contains(page_header, "tessera::ui::UpdateScheduler* scheduler_ = nullptr;"),
          "the owner keeps its lifecycle and scheduler so a late item joins them");
    check(contains(page_header, "Row child_5_;"),
          "a conditional instance stays a caller member (its state outlives the branch)");
    check(contains(page_header, "branch_5_owned_"),
          "the conditional instance has a branch ownership slot");
    check(!contains(page_header, "Row child_4_;"),
          "a per-item instance is not also a caller member");

    // Build: the item builder creates, props, builds, then joins the owner's lifecycle.
    check(contains(page_source, "tessera_out.child_4 = std::make_unique<Row>();"),
          "build_item_4 creates the instance");
    check(contains(page_source, "tessera_out.child_4->prop_label_ = entry;"),
          "the per-item prop is assigned before build()");
    check(contains(page_source, "tessera_out.child_4->build(*node_4);"),
          "the per-item instance builds against its host node");
    check(contains(page_source, "if (this->mounted_) {") &&
              contains(page_source, "tessera_out.child_4->mount();"),
          "an item created after mount starts mounted");
    check(contains(page_source, "tessera_out.child_4->attach_scheduler(*this->scheduler_);"),
          "an item created after attach joins the scheduler");

    // Per-item content pass: guarded props, per-item presence walk, then the text pass.
    check(contains(page_source, "apply_item_children_2(list_2_items_[tessera_slot], n, "
                                "tessera_index);"),
          "a conditional inside an item runs through the per-item walk");
    check(contains(page_source, "list_4_items_[tessera_slot].child_4_prop_label_hash_"),
          "the per-item prop is hash-guarded in the item record");
    check(contains(page_source, "std::move(tessera_old_items[tessera_match]);"),
          "keyed reuse moves the whole record, instances included");
    check(contains(page_source, "tessera_old_items[tessera_slot].child_4->unmount();"),
          "a removed key unmounts the item's instance before its subtree dies");

    // Lifecycle and scheduler propagation reach the live item instances (section 3).
    check(contains(page_source, "tessera_slot.child_4->mount();") ||
              contains(page_source, "tessera_slot.child_4->mount();"),
          "mount() mounts live item instances");
    check(contains(page_source, "tessera_slot.child_4->update();"),
          "update() updates live item instances");
    check(contains(page_source, "tessera_slot.child_4->detach_scheduler(scheduler);"),
          "detach_scheduler() leaves the live item instances");

    // A conditional instance in the fixed tree: built once, attached by the branch walk.
    check(contains(page_source, "this->child_5_.build(*node_5);"),
          "the conditional instance is built during build()");
    check(contains(page_source, "this->branch_5_owned_ = std::move(node_5);"),
          "the conditional instance host node goes to its branch slot");
    check(contains(page_source, "attach_child_at(parent, position, branch_5_owned_, "
                                "branch_5_node_);"),
          "the branch walk attaches the conditional instance at its source position");

    // Automation identity: the numeric item key is published on the item root.
    check(contains(page_source, "set_item_key("),
          "a numeric item key is published as the automation item key");

    // The callee maps its list-typed prop (form 4).
    check(contains(row_header, "std::vector<std::string> prop_notes_ = {};"),
          "the callee owns a list-typed prop as a generated vector");
    check(contains(file_text(*result, "Row.cpp"), "prop_notes_"),
          "the callee maps the list-typed prop");
}

void test_goldens(bool update) {
    const auto result = codegen::generate(instances_fixture::make_instances_module());
    check(result.has_value(), "the instance golden sample generates");
    if (!result) {
        return;
    }
    const std::string directory = TESSERA_CODEGEN_INSTANCES_GOLDEN_DIR;
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
            check(false,
                  file.path + " differs from its golden; run `codegen_instances_tests --write`");
        }
    }
    if (update) {
        std::cout << "instance goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = codegen::generate(instances_fixture::make_instances_module());
    const auto second = codegen::generate(instances_fixture::make_instances_module());
    check(first.has_value() && second.has_value(), "the instance sample generates twice");
    if (!first || !second) {
        return;
    }
    check(file_text(*first, "Page.cpp") == file_text(*second, "Page.cpp"),
          "two runs are byte-identical");
}

// The forms this slice lifts are accepted; the forms it does not cover stay loud.
void test_lifted_and_deferred_forms() {
    check(codegen::generate(instances_fixture::make_instances_module()).has_value(),
          "a keyed list of instances, a conditional instance and a conditional inside an item all "
          "generate");

    {
        // Slot content of a per-item instance would need a per-item slot table.
        ir::IrModule module = instances_fixture::make_instances_module();
        module.components[0].root.children[1].children.push_back(
            instances_fixture::node_of(ir::NodeKind::Text, "node::9", "2109", 260));
        const auto result = codegen::generate(module);
        check(!result.has_value(), "slot children of a per-item instance are deferred");
        check(!result.error().empty() &&
                  result.error().front().find("slot children") != std::string::npos,
              "the per-item slot children form is diagnosed");
    }
    {
        // A nested list template stays deferred.
        ir::IrModule module = instances_fixture::make_instances_module();
        ir::Node nested = instances_fixture::node_of(ir::NodeKind::View, "node::9", "2109", 260);
        nested.loop = ir::Loop{"inner", std::string(), instances_fixture::ref("state::entries")};
        nested.key = instances_fixture::ref("inner");
        module.components[0].root.children[0].children.push_back(std::move(nested));
        check(!codegen::generate(module).has_value(), "a nested list template is deferred");
    }
    {
        // A `useRef` target on a per-item instance cannot name one item's instance: the target is
        // rejected before any code is emitted.
        ir::IrModule module = instances_fixture::make_instances_module();
        module.components[0].refs.push_back({"useRef::row", "row", "node::4"});
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a per-item instance `useRef` target is deferred");
    }
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
    test_goldens(false);
    test_determinism();
    test_lifted_and_deferred_forms();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen instance tests passed\n";
    return 0;
}
