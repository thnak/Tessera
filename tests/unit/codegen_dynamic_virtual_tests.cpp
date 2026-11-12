// SPDX-License-Identifier: MIT

// Prop-driven virtual collection options (virtualization-v1.md section 10, docs/design/compiler.md
// section 11): `orientation`, `columns`, `overscan` and `columnWidth` accept a component prop
// reference instead of a literal. The generated component re-reads the prop at sync time, so a prop
// change re-realizes the window on the next `scroll_virtual_<n>()`, with no codegen change, no full
// component rebuild and no id/layout change to unrelated nodes.
//
// This test pins:
// - the emitted shape: a prop-driven option is a mutable member refreshed by the scroll entry
// point,
//   a literal keeps the exact `static constexpr` text (the committed `VirtualDemo.*` golden must
//   not move);
// - the runtime behavior: `columns` 2 -> 3 changes the row count and the realized cell indices,
//   `orientation` switches vertical <-> horizontal realization for the same props, a zero/negative
//   `columns` falls back to column-width mode and a negative `overscan` clamps to 0;
// - loud typed rejection of a computed expression, a state reference and a wrong prop type.
//
// Deliberate golden update path: `codegen_dynamic_virtual_tests --write` rewrites
// tests/golden/codegen/dynamic/DynamicVirtual.*; review the diff and rebuild.

#include "codegen_dynamic_virtual_module.hpp"
#include "codegen_virtual_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#ifdef TESSERA_DYNAMIC_VIRTUAL_SAMPLE
#include "DynamicVirtual.hpp"
#include "tessera/ui/scene/node.hpp"
#endif

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

using tessera::test::codegen_dynamic_virtual::integer;
using tessera::test::codegen_dynamic_virtual::make_dynamic_virtual_module;
using tessera::test::codegen_virtual::make_virtual_module;

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

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
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

// `root.children[1]` is the VirtualList, `root.children[2]` the VirtualGrid.
ir::Node& list_of(ir::IrModule& module) {
    return module.components[0].root.children[1];
}

ir::Node& grid_of(ir::IrModule& module) {
    return module.components[0].root.children[2];
}

void test_fixture_is_valid() {
    const ir::IrModule module = make_dynamic_virtual_module();
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the prop-driven virtual fixture is a valid IR module");
}

// The prop-driven emission shape: no `static constexpr` for the field, and one refresh statement
// per prop-driven option inside the generated scroll entry point.
void test_prop_driven_emission() {
    const auto result = codegen::generate(make_dynamic_virtual_module());
    check(result.has_value(), "the prop-driven fixture generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string& header = (*result)[0].text;
    const std::string& source = (*result)[1].text;
    check(contains(header, "bool virtual_3_horizontal_ = false; // prop-driven"),
          "a prop-driven orientation is a mutable member");
    check(contains(header, "double virtual_3_overscan_ = 200.0; // prop-driven"),
          "a prop-driven overscan is a mutable member seeded with the spec default");
    check(contains(header, "int virtual_6_columns_ = 0; // prop-driven"),
          "a prop-driven column count is a mutable member");
    check(contains(header, "double virtual_6_column_width_ = 0.0; // prop-driven"),
          "a prop-driven column width is a mutable member");
    check(!contains(header, "static constexpr bool virtual_3_horizontal_"),
          "a prop-driven orientation is not a compile-time constant");
    check(!contains(header, "static constexpr double virtual_3_overscan_"),
          "a prop-driven overscan is not a compile-time constant");
    check(!contains(header, "static constexpr int virtual_6_columns_"),
          "a prop-driven column count is not a compile-time constant");
    check(contains(source, "virtual_3_horizontal_ = (prop_orientation_ == \"horizontal\");"),
          "the scroll entry point reads the orientation prop");
    check(contains(source, "virtual_3_overscan_ = static_cast<double>(prop_overscan_);"),
          "the scroll entry point reads the overscan prop");
    check(contains(source, "virtual_6_columns_ = static_cast<int>(prop_columns_);"),
          "the scroll entry point reads the columns prop before the row math");
    check(contains(source, "virtual_6_column_width_ = static_cast<double>(prop_columnWidth_);"),
          "the scroll entry point reads the columnWidth prop");
    check(contains(source, "if (virtual_3_overscan_ < 0.0)"),
          "a negative prop overscan is clamped");
    check(contains(source, "if (virtual_6_columns_ < 0)"),
          "a negative prop column count is clamped");
    check(contains(source, "virtual_3_horizontal_ ? tessera_item->bounds().width"),
          "the measure pass picks the axis from the prop-driven orientation");
    check(contains(source, "const std::size_t tessera_columns = static_cast<std::size_t>("
                           "virtual_6_columns_);"),
          "the row mapping reads the refreshed column count");
    // The refresh runs before the row/window math: a changed prop must remap this same call.
    check(source.find("virtual_6_columns_ = static_cast<int>(prop_columns_);") <
              source.find("(tessera_total + tessera_columns - 1) / tessera_columns"),
          "the column refresh precedes the row-count computation");
}

// A literal still emits the exact recorded snippet: the structural members stay `static constexpr`.
void test_literal_emission_snippet() {
    ir::IrModule module = make_dynamic_virtual_module();
    list_of(module).props["orientation"] = ir::make_literal(std::string("horizontal"));
    list_of(module).props["overscan"] = integer(200);
    grid_of(module).props["columns"] = integer(4);
    grid_of(module).props["columnWidth"] = integer(80);
    const auto result = codegen::generate(module);
    check(result.has_value(), "the literal variant generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string& header = (*result)[0].text;
    const std::string& source = (*result)[1].text;
    for (const std::string_view snippet :
         {"static constexpr bool virtual_3_horizontal_ = true; // main axis is x instead of y",
          "static constexpr double virtual_3_overscan_ = 200.0;",
          "static constexpr int virtual_6_columns_ = 4; // 0 means column-width mode",
          "static constexpr double virtual_6_column_width_ = 80.0;"}) {
        check(contains(header, snippet),
              std::string("the literal snippet is unchanged: ") + std::string(snippet));
    }
    check(!contains(source, "prop_orientation_"), "a literal orientation reads no prop");
    check(!contains(source, "prop_overscan_"), "a literal overscan reads no prop");
    check(!contains(source, "prop_columns_"), "a literal column count reads no prop");
}

// The existing literal golden must not move: the literal path is byte-identical to the committed
// `VirtualDemo.*` (the old fixture has literal structural options and state/prop size inputs).
void test_literal_golden_unchanged() {
    const auto result = codegen::generate(make_virtual_module());
    check(result.has_value(), "the literal fixture still generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string directory = TESSERA_CODEGEN_GOLDEN_DIR;
    for (const codegen::GeneratedFile& file : *result) {
        const std::string golden = read_file(directory + "/" + file.path);
        check(!golden.empty(), "golden " + file.path + " is readable");
        check_eq(golden, file.text,
                 file.path + " literal golden is unchanged by the prop-driven slice");
    }
}

void test_goldens(bool update) {
    const auto result = codegen::generate(make_dynamic_virtual_module());
    check(result.has_value(), "the prop-driven golden sample generates");
    if (!result) {
        return;
    }
    const std::string directory = std::string(TESSERA_CODEGEN_GOLDEN_DIR) + "/dynamic";
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
        check_eq(golden, file.text,
                 file.path + " golden matches (codegen_dynamic_virtual_tests --write)");
    }
    if (update) {
        std::cout << "dynamic virtual codegen goldens written to " << directory << "\n";
    }
}

// A prop-driven column count still supports a collection event: the generated row mapping reads the
// refreshed member, so the event wiring does not need a literal count.
void test_prop_driven_columns_with_event() {
    ir::IrModule module = make_dynamic_virtual_module();
    ir::Handler handler;
    handler.id = "handler::dynamic_end";
    grid_of(module).events.emplace("onEndReached", "handler::dynamic_end");
    module.components[0].handlers.push_back(std::move(handler));
    const auto result = codegen::generate(module);
    check(result.has_value(), "a prop-driven column count with a collection event generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    check(contains((*result)[1].text, "this->handler_dynamic_end(tessera_end_event);"),
          "the collection event is dispatched with the prop-driven row mapping");
}

void test_rejections() {
    const auto expect_error = [](const ir::IrModule& module, std::string_view what,
                                 std::string_view needle) {
        const auto result = codegen::generate(module);
        check(!result.has_value(), what);
        const std::string error = first_error(result);
        check(error.rfind("TESSERA_CODEGEN_", 0) == 0,
              std::string(what) + " reports a TESSERA_CODEGEN_ code");
        check(contains(error, needle),
              std::string(what) + " names the offending prop: " + std::string(needle));
    };
    {
        ir::IrModule module = make_dynamic_virtual_module();
        grid_of(module).props["columns"] =
            ir::make_binary("+", ir::make_ref("prop::columns"), integer(1));
        expect_error(module, "a computed column count is rejected", "columns");
    }
    {
        ir::IrModule module = make_dynamic_virtual_module();
        grid_of(module).props["columns"] = ir::make_ref("prop::orientation");
        expect_error(module, "a string column count prop is rejected", "type `string`");
    }
    {
        ir::IrModule module = make_dynamic_virtual_module();
        list_of(module).props["orientation"] = ir::make_ref("prop::columns");
        expect_error(module, "an int orientation prop is rejected", "type `int`");
    }
    {
        ir::IrModule module = make_dynamic_virtual_module();
        list_of(module).props["overscan"] = ir::make_ref("prop::orientation");
        expect_error(module, "a string overscan prop is rejected", "type `string`");
    }
    {
        ir::IrModule module = make_dynamic_virtual_module();
        grid_of(module).props["columnWidth"] = ir::make_ref("prop::orientation");
        expect_error(module, "a string column width prop is rejected", "columnWidth");
    }
    {
        ir::IrModule module = make_dynamic_virtual_module();
        list_of(module).props["overscan"] = ir::make_ref("state::items");
        expect_error(module, "a state-driven structural option is rejected", "props.<name>");
    }
    {
        ir::IrModule module = make_dynamic_virtual_module();
        list_of(module).props["orientation"] = ir::make_literal(std::string("diagonal"));
        expect_error(module, "an unknown literal orientation is rejected", "orientation");
    }
}

#ifdef TESSERA_DYNAMIC_VIRTUAL_SAMPLE

namespace gen = dvirt::gen;
namespace ui = tessera::ui;
namespace rhi = tessera::rendering::rhi;

gen::DynamicVirtual build_component(ui::Node& scene) {
    gen::DynamicVirtual component;
    component.build(scene);
    return component;
}

// columns 2 -> 3: the row count and the realized absolute cell indices follow the prop on the next
// sync, and an unrelated node keeps its id and identity.
void test_columns_prop_realiases_window() {
    ui::Node scene(1000);
    gen::DynamicVirtual component = build_component(scene);
    check(scene.children().size() == 1, "the component builds one root child");
    if (scene.children().empty()) {
        return;
    }
    ui::Node& root = *scene.children()[0];
    ui::Node* const header = root.children()[0].get();
    const ui::NodeId header_id = header->id();
    const ui::Dirty footer_dirty = root.children()[3]->dirty();
    component.prop_orientation_ = "vertical";
    component.prop_overscan_ = 0;
    component.prop_columns_ = 2;
    float scroll = 0.0F;
    check(component.scroll_virtual_6(scroll, 240.0F), "the two-column grid scroll applies");
    check(component.virtual_6_columns_ == 2, "the generated member holds the prop column count");
    check(component.virtual_6_geometry_count_ == 5, "10 items in 2 columns are 5 rows");
    check(component.virtual_6_first_ == 0 && component.virtual_6_count_ == 6,
          "rows 0..2 realize the six cells 0..5");

    component.prop_columns_ = 3;
    check(component.scroll_virtual_6(scroll, 240.0F), "the changed-column scroll applies");
    check(component.virtual_6_columns_ == 3, "the next sync adopts the new column count");
    check(component.virtual_6_geometry_count_ == 4, "10 items in 3 columns are 4 rows");
    check(component.virtual_6_first_ == 0 && component.virtual_6_count_ == 9,
          "rows 0..2 realize the nine cells 0..8");
    check(component.virtual_6_items_[8].root->id() == static_cast<ui::NodeId>(7 + 8 * 10),
          "a realized grid cell keeps its absolute-index NodeId");
    check(root.children()[0].get() == header && header->id() == header_id,
          "an unrelated header node keeps its id and identity");
    check(root.children()[3]->dirty() == footer_dirty,
          "an unrelated footer node is not newly marked dirty by the re-realization");
}

// orientation vertical -> horizontal: for the same props change the measure pass reads the other
// axis, so the realized range and the recorded main extent both change.
void test_orientation_prop_switches_axis() {
    ui::Node scene(1000);
    gen::DynamicVirtual component = build_component(scene);
    component.prop_orientation_ = "vertical";
    component.prop_overscan_ = 0;
    float scroll = 0.0F;
    check(component.scroll_virtual_3(scroll, 250.0F), "the vertical list scroll applies");
    check(!component.virtual_3_horizontal_, "a vertical prop keeps the y axis");
    check(component.virtual_3_count_ == 3, "the vertical window realizes items 0..2");
    // One realized item reports an extent that differs per axis: 200 tall, 50 wide.
    component.virtual_3_items_[0].root->set_bounds(rhi::Rect{0.0F, 0.0F, 50.0F, 200.0F});
    check(component.scroll_virtual_3(scroll, 250.0F), "the measured vertical scroll applies");
    check_eq(std::to_string(component.virtual_3_geometry_.item_main_size(0)),
             std::to_string(200.0F), "vertical orientation measures the item height");
    check(component.virtual_3_count_ == 2, "the taller item shrinks the vertical window to 0..1");

    component.prop_orientation_ = "horizontal";
    check(component.scroll_virtual_3(scroll, 250.0F), "the horizontal list scroll applies");
    check(component.virtual_3_horizontal_, "the prop-driven orientation switched to x");
    check_eq(std::to_string(component.virtual_3_geometry_.item_main_size(0)), std::to_string(50.0F),
             "horizontal orientation measures the item width");
    check(component.virtual_3_count_ == 3, "the narrower item grows the horizontal window to 0..2");

    component.prop_orientation_ = "diagonal";
    check(component.scroll_virtual_3(scroll, 250.0F), "an unknown orientation does not crash");
    check(!component.virtual_3_horizontal_,
          "an unknown orientation falls back to vertical (the documented default)");
}

// Runtime validation follows the existing runtime contract: a non-positive column count is the
// column-width mode 0 (the generated row mapping refuses, the host drives the window), and a
// negative overscan clamps to 0 (no overscan).
void test_runtime_validation() {
    ui::Node scene(1000);
    gen::DynamicVirtual component = build_component(scene);
    component.prop_orientation_ = "vertical";
    component.prop_overscan_ = 0;
    component.prop_columns_ = 2;
    float scroll = 0.0F;
    check(component.scroll_virtual_6(scroll, 240.0F), "the baseline grid scroll applies");
    const std::size_t rows = component.virtual_6_geometry_count_;
    check(rows == 5, "the baseline grid has 5 rows");

    component.prop_columns_ = 0;
    check(!component.scroll_virtual_6(scroll, 240.0F),
          "a zero column count refuses the generated row mapping");
    check(component.virtual_6_columns_ == 0, "zero columns stay in column-width mode");
    check(component.virtual_6_geometry_count_ == rows,
          "a refused sync leaves the row count untouched");

    component.prop_columns_ = -3;
    check(!component.scroll_virtual_6(scroll, 240.0F),
          "a negative column count refuses the generated row mapping");
    check(component.virtual_6_columns_ == 0,
          "a negative column count clamps to column-width mode 0");
    check(component.virtual_6_geometry_count_ == rows,
          "the clamped sync leaves the row count untouched");

    component.prop_overscan_ = -100;
    check(component.scroll_virtual_3(scroll, 100.0F), "a negative overscan does not crash");
    check_eq(std::to_string(component.virtual_3_overscan_), std::to_string(0.0),
             "a negative overscan clamps to 0");
    check(component.virtual_3_count_ == 1, "no overscan realizes only the visible item");

    component.prop_overscan_ = 200;
    check(component.scroll_virtual_3(scroll, 100.0F), "the positive overscan scroll applies");
    check(component.virtual_3_count_ == 3, "a positive overscan extends the realized window");
}

#endif // TESSERA_DYNAMIC_VIRTUAL_SAMPLE

void test_determinism() {
    const auto first = codegen::generate(make_dynamic_virtual_module());
    const auto second = codegen::generate(make_dynamic_virtual_module());
    check(first.has_value() && second.has_value(), "the prop-driven sample generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second), "two runs are byte-identical");
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
    test_prop_driven_emission();
    test_literal_emission_snippet();
    test_literal_golden_unchanged();
    test_goldens(false);
    test_prop_driven_columns_with_event();
    test_rejections();
    test_determinism();
#ifdef TESSERA_DYNAMIC_VIRTUAL_SAMPLE
    test_columns_prop_realiases_window();
    test_orientation_prop_switches_axis();
    test_runtime_validation();
#endif
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "dynamic virtual option tests passed\n";
    return 0;
}
