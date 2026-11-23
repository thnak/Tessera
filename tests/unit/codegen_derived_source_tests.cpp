// SPDX-License-Identifier: MIT

// Issue #830: a derived `.map` source (`listFilter`/`listSlice` of a list ref). Split from
// `codegen_dynamic_tests.cpp` at the 500-line limit. Proves the source is accepted, its element
// type comes from the innermost ref, and it is materialized into a cached member under the loop's
// own dirty mask.

#include "codegen_dynamic_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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

using tessera::test::codegen_dynamic::integer;
using tessera::test::codegen_dynamic::make_filtered_module;

codegen::CodegenResult generate_base(const ir::IrModule& module) {
    return codegen::generate(module, {}, std::nullopt);
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

// Issue #830: a derived list source is accepted, its element type comes from the innermost ref, and
// the source is materialized into a cached member under the loop's own dirty mask.
void test_derived_source_shape() {
    const ir::IrModule module = make_filtered_module();
    check(ir::validate(module).empty(), "the derived-source fixture is a valid IR module");
    const auto result = generate_base(module);
    check(result.has_value(), "a derived list source generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string& text = (*result)[0].text;
    check(text.find("std::vector<std::string> list_2_src_{};") != std::string::npos,
          "the derived source is a cached member of the element type");
    check(text.find("list_2_src_ = tessera::ui::filter_list(") != std::string::npos,
          "apply_state() materializes the derived source");
    check(text.find("if (dirty_.intersects(kDirty_list_2))") != std::string::npos,
          "the cache refresh is gated on the loop's input mask");
    check(text.find(": list_2_src_)") != std::string::npos,
          "the diff passes range over the cached member");
    check(text.find("TESSERA_LIST_DUPLICATE_KEY(this, \"node::2\", tessera_key);") !=
              std::string::npos,
          "a duplicate live key is reported, then made unique by position");
    check(text.find("#include \"tessera/ui/scene/list_diagnostics.hpp\"") != std::string::npos,
          "the diagnostic hook header is included for a keyed list");
    check(text.find("#include \"tessera/ui/scene/list_ops.hpp\"") != std::string::npos,
          "the list helper header is included for a `filter` source");
}

// The allowed source shapes: a `filter`, a `slice`, and a `filter` of a `slice` (analysis and
// element-type inference recurse to the innermost ref), and a `slice` of a `filter`.
void test_derived_source_shapes() {
    // `rows.filter(row => row.startsWith(query))`
    {
        ir::IrModule module = make_filtered_module();
        const auto result = generate_base(module);
        check(result.has_value(), "a `filter` source generates");
    }
    // `rows.slice(0, 2)`
    {
        ir::IrModule module = make_filtered_module();
        module.components[0].root.children[0].loop->source = ir::make_list_slice(
            ir::make_ref("prop::rows"), [] {
                std::vector<ir::ExprPtr> bounds;
                bounds.push_back(integer(0));
                bounds.push_back(integer(2));
                return bounds;
            }());
        const auto result = generate_base(module);
        check(result.has_value(), "a `slice` source generates");
        if (result) {
            check((*result)[0].text.find("tessera::ui::slice_list(prop_rows_, 0, 2)") !=
                      std::string::npos,
                  "a `slice` source emits the slice helper over the ref");
        }
    }
    // `rows.slice(0, 2).filter(row => row.startsWith(query))`
    {
        ir::IrModule module = make_filtered_module();
        std::vector<ir::ExprPtr> bounds;
        bounds.push_back(integer(0));
        bounds.push_back(integer(2));
        std::vector<ir::ExprPtr> predicate_args;
        predicate_args.push_back(ir::make_ref("row"));
        predicate_args.push_back(ir::make_ref("prop::query"));
        module.components[0].root.children[0].loop->source = ir::make_list_filter(
            ir::make_list_slice(ir::make_ref("prop::rows"), std::move(bounds)), "row",
            ir::make_call("startsWith", std::move(predicate_args)));
        const auto result = generate_base(module);
        check(result.has_value(), "a `filter` of a `slice` source generates");
        if (result) {
            check((*result)[0].text.find("tessera::ui::filter_list(tessera::ui::slice_list(") !=
                      std::string::npos,
                  "the nested derivation emits the inner slice inside the outer filter");
        }
    }
}

} // namespace

int main() {
    test_derived_source_shape();
    test_derived_source_shapes();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "derived-source codegen tests passed\n";
    return 0;
}
