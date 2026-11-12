// SPDX-License-Identifier: MIT

// Dynamic virtual geometry (ir-v1.md section 3.5, virtualization-v1.md sections 6 and 10,
// docs/design/compiler.md section 11): the numeric geometry inputs (`estimateSize`, `gap`,
// `itemSize`, `rowHeight`) may come from a state/prop cell instead of a literal. This test pins the
// emitted re-seed contract:
//
// - one `update_virtual_geometry_<n>(scroll, viewport)` per virtual collection, hash-guarded so a
//   per-frame call re-seeds once per change and never rebuilds an unchanged geometry;
// - an `estimateSize`/`gap` change re-seeds in place (`ItemGeometry::reseed`), keeping every
//   measurement; a changed declared fixed extent (`itemSize`/`rowHeight`) additionally drops them,
//   but only when that extent seeds the estimate (no `estimateSize` overriding it);
// - the re-anchor runs through the runtime section 6 rule with the old/new prefix sums;
// - a non-numeric or computed geometry value is a loud TESSERA_CODEGEN_UNSUPPORTED, and the
//   structural options (`orientation`/`columns`/`overscan`/`columnWidth`) reject every dynamic form
//   except a component prop reference (the prop-driven slice has its own
//   `codegen_dynamic_virtual_options` test).

#include "codegen_virtual_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ir = tessera::compiler::ir;
namespace codegen = tessera::compiler::codegen;

using tessera::test::codegen_virtual::integer;
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

// The fixture drives the list estimate/gap from state, its fixed extent from a prop, and the grid
// row height from state, so every emitted input has a dynamic and a hashed form.
void test_reseed_shape() {
    const auto result = codegen::generate(make_virtual_module());
    check(result.has_value(), "the dynamic-geometry fixture generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string& header = (*result)[0].text;
    const std::string& source = (*result)[1].text;
    for (const std::string_view member :
         {"double virtual_3_item_size_ = static_cast<double>(prop_item_extent_);",
          "double virtual_3_estimate_size_ = static_cast<double>(state_row_extent_);",
          "double virtual_3_gap_ = static_cast<double>(state_row_gap_);",
          "double virtual_6_row_height_ = static_cast<double>(state_grid_row_);"}) {
        check(contains(header, member),
              std::string("the re-seeded member is emitted: ") + std::string(member));
    }
    check(contains(header, "bool update_virtual_geometry_3(float& tessera_scroll, "
                           "float tessera_viewport);"),
          "the re-seed method is declared");
    check(contains(header, "bool update_virtual_geometry_6(float& tessera_scroll, "
                           "float tessera_viewport);"),
          "the grid declares its own re-seed method");
    check(contains(header, "std::uint64_t virtual_3_geometry_seeds_ = 0;"),
          "the per-collection re-seed counter is a generated member");
    check(contains(source, "bool VirtualDemo::update_virtual_geometry_3(float& tessera_scroll, "
                           "float tessera_viewport)"),
          "the re-seed method is defined");
    check(contains(source, "std::uint64_t tessera_hash = tessera::ui::kEffectHashSeed;"),
          "the re-seed guard hashes the inputs");
    check(contains(source, "if (virtual_3_geometry_set_ && tessera_hash == "
                           "virtual_3_geometry_hash_)"),
          "an unchanged geometry returns before rebuilding");
    check(contains(source, "virtual_3_geometry_hash_ = tessera_hash;") &&
              contains(source, "virtual_3_geometry_set_ = true;") &&
              contains(source, "++virtual_3_geometry_seeds_;"),
          "the re-seed publishes the hash and counts once per change");
    check(contains(source, "virtual_3_geometry_.reseed(static_cast<float>(tessera_estimate),"),
          "the estimate/gap change re-seeds the geometry in place");
    check(!contains(source, "virtual_3_geometry_.clear_measurements();"),
          "an estimate/gap change keeps the measurements");
    check(contains(source, "const bool tessera_drop_measures ="),
          "a changed declared fixed extent invalidates measurements");
    check(contains(source, "virtual_6_geometry_.clear_measurements();"),
          "the grid drops its row measurements when the row height changes");
    check(contains(source, "tessera::ui::virtualization::reanchor_scroll("),
          "the re-seed re-anchors with the runtime section 6 rule");
    check(contains(source, "tessera_scroll = tessera_adjust.scroll_offset;"),
          "the re-anchored offset is written back to the caller");
    check(contains(source, "update_virtual_geometry_3(tessera_scroll, tessera_viewport);"),
          "scroll_virtual_<n> runs the re-seed path before the measure pass");
    check(contains(source, "update_virtual_geometry_6(tessera_scroll, tessera_viewport);"),
          "the grid scroll entry point runs it too");
}

// A literal geometry keeps the same shape: the seeded member holds the literal, the re-seed path
// re-applies it, and only a fixed extent that actually seeds the estimate drops measurements.
void test_literal_geometry() {
    ir::IrModule module = make_virtual_module();
    ir::Node& list = module.components[0].root.children[1];
    list.props.erase("estimateSize");
    list.props["itemSize"] = integer(48);
    ir::Node& grid = module.components[0].root.children[2];
    grid.props["rowHeight"] = integer(36);
    grid.props["estimateSize"] = integer(30);
    const auto result = codegen::generate(module);
    check(result.has_value(), "the literal-geometry variant generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string& header = (*result)[0].text;
    const std::string& source = (*result)[1].text;
    check(contains(header, "double virtual_3_item_size_ = 48.0;"),
          "the fixed item size is the emitted seed");
    check(contains(header, "double virtual_3_estimate_size_ = 48.0;"),
          "itemSize seeds the effective estimate when estimateSize is absent");
    check(contains(source, "const double tessera_estimate = 48.0;"),
          "the re-seed path re-applies the literal estimate");
    check(contains(source, "virtual_3_geometry_.clear_measurements();"),
          "a list fixed extent that seeds the estimate drops measurements when it changes");
    check(contains(header, "double virtual_6_estimate_size_ = 30.0;"),
          "estimateSize overrides the grid row height as the estimate");
    check(!contains(source, "virtual_6_geometry_.clear_measurements();"),
          "rowHeight is informational while estimateSize overrides it, so nothing is dropped");
    check(!contains(source, "tessera_fixed != virtual_6_row_height_"),
          "no invalidation comparison is emitted for an informational fixed extent");
}

void test_rejections() {
    const auto expect_error = [](const ir::IrModule& module, std::string_view what) {
        const auto result = codegen::generate(module);
        check(!result.has_value(), what);
        check(first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              std::string(what) + " reports a TESSERA_CODEGEN_ code");
    };
    const auto with_list_prop = [](std::string name, ir::ExprPtr value) {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].props[std::move(name)] = std::move(value);
        return module;
    };
    expect_error(with_list_prop("orientation", ir::make_ref("state::row_gap")),
                 "a state-driven orientation is rejected (only a component prop is prop-driven)");
    expect_error(with_list_prop("overscan", ir::make_ref("state::row_gap")),
                 "a state-driven overscan is rejected (only a component prop is prop-driven)");
    expect_error(with_list_prop("estimateSize", ir::make_ref("state::items")),
                 "a non-numeric geometry cell is rejected");
    expect_error(with_list_prop("gap", ir::make_binary("*", integer(2), integer(3))),
                 "a computed geometry expression is deferred");
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[2].props["columns"] = ir::make_ref("state::row_gap");
        expect_error(module,
                     "a state-driven grid column count is rejected (only a prop is dynamic)");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[2].props["columnWidth"] = ir::make_ref("state::row_gap");
        expect_error(module, "a state-driven column width is rejected");
    }
}

void test_determinism() {
    const auto first = codegen::generate(make_virtual_module());
    const auto second = codegen::generate(make_virtual_module());
    check(first.has_value() && second.has_value(), "the dynamic sample generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second), "two runs are byte-identical");
    }
}

} // namespace

int main() {
    test_reseed_shape();
    test_literal_geometry();
    test_rejections();
    test_determinism();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen virtual geometry tests passed\n";
    return 0;
}
