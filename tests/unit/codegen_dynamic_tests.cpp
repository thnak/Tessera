// SPDX-License-Identifier: MIT

// Conditional and keyed-list codegen tests on the ADR-040 generated base (docs/design/compiler.md
// section 11): generated shape, byte-identical determinism, the `Dynamic.g.hpp` golden, and loud
// rejection of the deferred dynamic forms.
//
// The `Dynamic` fixture is hand-built IR: the real `.tsx` front end cannot author a list yet (a
// list-typed `useState` initial value is rejected by lowering, see
// lowering_hooks_reject_tests.cpp), so the list contract is proved against IR directly.
//
// Deliberate golden update path: `codegen_dynamic_tests --write` rewrites
// tests/golden/codegen/Dynamic.g.hpp; then review the diff and rebuild.

#include "codegen_dynamic_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

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

using tessera::test::codegen_dynamic::integer;
using tessera::test::codegen_dynamic::item_text;
using tessera::test::codegen_dynamic::leaf;
using tessera::test::codegen_dynamic::make_dynamic_module;
using tessera::test::codegen_dynamic::span;

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

codegen::CodegenResult generate_base(const ir::IrModule& module) {
    return codegen::generate(module, {}, std::nullopt, codegen::EmitMode::GeneratedBase);
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

std::string concatenated(const std::vector<codegen::GeneratedFile>& files) {
    std::string out;
    for (const codegen::GeneratedFile& file : files) {
        out += file.path + "\n" + file.text;
    }
    return out;
}

void test_fixture_is_valid() {
    const ir::IrModule module = make_dynamic_module();
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the dynamic codegen fixture is a valid IR module");
}

void test_generated_shape() {
    const auto result = generate_base(make_dynamic_module());
    check(result.has_value(), "the dynamic sample generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    check(result->size() == 1, "one header-only file per component");
    if (result->size() != 1) {
        return;
    }
    check_eq((*result)[0].path, "Dynamic.g.hpp", "deterministic generated-base path");
    const std::string& header = (*result)[0].text;
    const std::string& source = header; // the base is header-only: one text holds both halves
    check(header.find("kMaxGeneratedListItems") == std::string::npos,
          "no fixed item cap is emitted for keyed lists");
    check(header.find("struct Item_5 {") != std::string::npos, "the per-list item node struct");
    check(header.find("std::array<tessera::ui::Node*, 1> texts{};") != std::string::npos,
          "the item struct records its interpolated text nodes");
    check(header.find("std::unique_ptr<tessera::ui::Node> branch_3_owned_;") != std::string::npos,
          "a conditional branch owns its detached subtree");
    check(header.find("tessera::ui::Node* branch_3_node_ = nullptr;") != std::string::npos,
          "a conditional branch tracks its attached node");
    check(header.find("std::uint64_t branch_3_hash_ = 0;") != std::string::npos,
          "a conditional branch hashes its condition");
    check(header.find("tessera::ui::Node* children_parent_1_ = nullptr;") != std::string::npos,
          "the dynamic child group keeps its parent node");
    check(header.find("std::vector<Dynamic_detail::Item_5> list_5_items_{};") != std::string::npos,
          "the growable list item table");
    check(header.find("std::vector<std::string> list_5_keys_{};") != std::string::npos,
          "the key table");
    check(header.find("void apply_children_1();") != std::string::npos,
          "the child-set update is declared");
    check(header.find("std::unique_ptr<tessera::ui::Node> build_item_5(const std::string& item, "
                      "std::size_t tessera_index, std::size_t tessera_id_slot, "
                      "Dynamic_detail::Item_5& tessera_out);") != std::string::npos,
          "the per-item builder signature");
    check(header.find("void apply_list_5(tessera::ui::Node& parent, std::size_t insert_at);") !=
              std::string::npos,
          "the list update signature");
    check(source.find("DynamicBase<Self>::apply_state()") != std::string::npos, "apply_state");
    check(source.find("apply_children_1();") != std::string::npos,
          "apply_state drives the dynamic child set");
    check(source.find("constexpr std::uint64_t kItemIdStride = 13ull;") != std::string::npos,
          "item ids use a deterministic stride past the component's pre-order ids");
    check(source.find("void attach_child_at(tessera::ui::Node& parent, std::size_t index,") !=
              std::string::npos,
          "the attach helper is generated");
    check(source.find("void detach_child(tessera::ui::Node& parent,") != std::string::npos,
          "the detach helper is generated");
    check(source.find("attach_child_at(parent, position, branch_3_owned_, branch_3_node_);") !=
              std::string::npos,
          "a present branch attaches at its source position");
    check(source.find("detach_child(parent, branch_3_owned_, branch_3_node_);") !=
              std::string::npos,
          "an absent branch detaches");
    check(source.find("apply_list_5(parent, position);") != std::string::npos,
          "the list advances the child position by its item count");
    check(source.find("position += list_5_count_;") != std::string::npos,
          "the list item count is added to the position");
    check(source.find("std::uint64_t tessera_order = tessera::ui::kEffectHashSeed;") !=
              std::string::npos,
          "the list hashes its key order first");
    check(source.find("tessera_order = tessera::ui::effect_hash(tessera_order, "
                      "static_cast<int>(tessera_position));") != std::string::npos,
          "the order hash includes the item count");
    check(source.find("std::string tessera_key = format_value(item);") != std::string::npos,
          "keys are formatted only on a structural change");
    check(source.find("tessera_next_items[tessera_next] = tessera_old_items[tessera_match];") !=
              std::string::npos,
          "a matching key reuses the existing item node");
    check(source.find("build_item_5(item, tessera_next, tessera_id_slot, "
                      "tessera_next_items[tessera_next]);") != std::string::npos,
          "a new key builds one item");
    check(source.find("parent.insert_child(insert_at + tessera_next,") != std::string::npos,
          "items are inserted at their deterministic position");
    check(source.find("DynamicBase<Self>::build_item_5(") != std::string::npos,
          "the item builder is emitted");
    check(source.find("+ tessera_id_slot * kItemIdStride)") != std::string::npos,
          "item node ids are offset by the item's id slot");
    check(source.find("tessera_id_used[tessera_old_items[tessera_slot].id_slot] = true;") !=
              std::string::npos,
          "a new item takes an id slot no live item holds");
    check(source.find("apply_item_5(tessera_slot, item, tessera_slot);") != std::string::npos,
          "the content pass runs per item");
    check(source.find("if (!list_5_set_[tessera_slot] || tessera_hash != "
                      "list_5_hashes_[tessera_slot])") != std::string::npos,
          "item content is hash-compared");
}

void test_array_initial_reaches_list_path() {
    // End to end through the IR contract: a front end that lowers `useState<string[]>(["alpha",
    // "beta"])` produces an `array` initial (docs/specs/ir-v1.md section 4), which codegen must
    // emit as a braced list initializer while keeping the generated list update path.
    ir::IrModule module = make_dynamic_module();
    std::vector<ir::ExprPtr> items;
    items.push_back(ir::make_literal(std::string("alpha")));
    items.push_back(ir::make_literal(std::string("beta")));
    module.components[0].state[1].initial = ir::make_array(std::move(items));
    const auto result = generate_base(module);
    check(result.has_value(), "an array list initial generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string& header = (*result)[0].text;
    check(header.find("std::vector<std::string> state_items_ = {\"alpha\", \"beta\"};") !=
              std::string::npos,
          "the array initial becomes a braced C++ list initializer");
    check(header.find("void apply_list_5(") != std::string::npos,
          "the list state stays on the generated list path");
    // An array initial on a non-list field would emit text that cannot compile: reject it loudly.
    ir::IrModule scalar = make_dynamic_module();
    scalar.components[0].state[0].initial = ir::make_array(std::vector<ir::ExprPtr>{});
    const auto scalar_result = generate_base(scalar);
    check(!scalar_result.has_value() &&
              first_error(scalar_result).rfind("TESSERA_CODEGEN_", 0) == 0,
          "an array initial on a scalar state is rejected");
}

void test_goldens(bool update) {
    const auto result = generate_base(make_dynamic_module());
    check(result.has_value(), "the dynamic golden sample generates");
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
        check_eq(golden, file.text, file.path + " golden matches (codegen_dynamic_tests --write)");
    }
    if (update) {
        std::cout << "dynamic codegen goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = generate_base(make_dynamic_module());
    const auto second = generate_base(make_dynamic_module());
    check(first.has_value() && second.has_value(), "the dynamic sample generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second), "two runs are byte-identical");
    }
}

void test_rejections() {
    const auto expect_error = [](const ir::IrModule& module, std::string_view what) {
        const auto result = generate_base(module);
        check(!result.has_value(), what);
        check(first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              std::string(what) + " reports a TESSERA_CODEGEN_ code");
    };
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].root.condition = ir::make_literal(true);
        expect_error(module, "a conditional component root is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].root.loop = ir::Loop{"item", "", ir::make_ref("state::items")};
        module.components[0].root.key = ir::make_ref("item");
        expect_error(module, "a loop component root is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        // A conditional nested in a conditional branch is supported (only templates restrict it).
        ir::Node nested = leaf(ir::NodeKind::Text, "node::13", "13");
        nested.condition = ir::make_literal(true);
        nested.text = ir::make_literal(std::string("Nested"));
        module.components[0].root.children[4].children.push_back(std::move(nested));
        const auto result = generate_base(module);
        check(result.has_value(), "a conditional inside a conditional branch generates");
    }
    {
        ir::IrModule module = make_dynamic_module();
        // A nested list inside the list template.
        ir::Node inner = leaf(ir::NodeKind::Text, "node::13", "13");
        inner.text = ir::make_ref("item");
        inner.key = ir::make_ref("item");
        inner.loop = ir::Loop{"inner", "", ir::make_ref("state::items")};
        module.components[0].root.children[3].children.push_back(std::move(inner));
        expect_error(module, "a nested list template is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        // A TextInput inside the list template needs per-item edit state.
        ir::Node input = leaf(ir::NodeKind::TextInput, "node::13", "13");
        input.props.emplace("value", ir::make_ref("item"));
        module.components[0].root.children[3].children.push_back(std::move(input));
        expect_error(module, "a TextInput inside a list template is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        // Dynamic metadata inside the template needs per-item storage.
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::expr_part(ir::make_ref("item")));
        module.components[0].root.children[3].props.emplace("className",
                                                            ir::make_template(std::move(parts)));
        expect_error(module, "dynamic metadata inside a list template is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].root.children[3].style.emplace("width", ir::make_ref("state::count"));
        expect_error(module, "a dynamic inline style inside a list template is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].refs.push_back({"ref::row", "row", "node::5"});
        expect_error(module, "a useRef targeting a node inside a list template is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].state[1].type = "int";
        expect_error(module, "a list source that is not a list<T> is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].state[1].type = "list<Row>";
        expect_error(module, "a record element type has no C++ form in this slice");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].state[1].id = "state::other";
        expect_error(module, "a list source without a declared type is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].root.children[3].loop->item = "tessera_item";
        expect_error(module, "a loop local that shadows generated code names is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].root.children[3].loop->source =
            ir::make_binary("+", ir::make_ref("state::items"), ir::make_ref("state::items"));
        expect_error(module, "a non-reference loop source is rejected");
    }
    {
        ir::IrModule module = make_dynamic_module();
        module.components[0].root.children[3].key = ir::make_ref("state::count");
        expect_error(module, "a key expression outside its loop locals is rejected");
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
    test_array_initial_reaches_list_path();
    test_goldens(false);
    test_determinism();
    test_rejections();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "dynamic codegen tests passed\n";
    return 0;
}
