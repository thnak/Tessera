// SPDX-License-Identifier: MIT

// VirtualList/VirtualGrid codegen tests (ir-v1.md section 3.5, virtualization-v1.md section 10,
// docs/design/compiler.md section 11): the item-template builder, the host-range application path,
// the bounded window table, stable absolute-index NodeId slots, byte-identical determinism, loud
// rejection of the deferred forms, and merger idempotency over the generated output.
//
// Deliberate golden update path: `codegen_virtual_tests --write` rewrites
// tests/golden/codegen/VirtualDemo.*; then review the diff and rebuild.

#include "codegen_virtual_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/merger.hpp"

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

using tessera::test::codegen_virtual::integer;
using tessera::test::codegen_virtual::leaf;
using tessera::test::codegen_virtual::make_virtual_module;
using tessera::test::codegen_virtual::span;

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

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

void test_fixture_is_valid() {
    const ir::IrModule module = make_virtual_module();
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the virtual codegen fixture is a valid IR module");
}

void test_generated_shape() {
    const auto result = codegen::generate(make_virtual_module());
    check(result.has_value(), "the virtual sample generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    check(result->size() == 2, "one header and one source");
    if (result->size() != 2) {
        return;
    }
    check_eq((*result)[0].path, "VirtualDemo.hpp", "deterministic header path");
    check_eq((*result)[1].path, "VirtualDemo.cpp", "deterministic source path");
    const std::string& header = (*result)[0].text;
    const std::string& source = (*result)[1].text;
    check(!contains(header, "kMaxGeneratedVirtualItems"),
          "no fixed realized-window cap is emitted");
    check(contains(header, "struct Item_4 {"), "the item node struct of the list template");
    check(contains(header, "struct Item_7 {"), "the item node struct of the grid template");
    check(contains(header, "std::array<tessera::ui::Node*, 1> texts{};"),
          "the item struct records its interpolated text node");
    check(contains(header, "static constexpr double virtual_3_estimate_size_ = 32.0;"),
          "the list estimate is a generated constant");
    check(contains(header, "static constexpr double virtual_3_gap_ = 4.0;"),
          "the list gap is a generated constant");
    check(contains(header, "static constexpr double virtual_3_overscan_ = 200.0;"),
          "the spec default overscan is a generated constant");
    check(contains(header, "static constexpr double virtual_3_item_size_ = 0.0;"),
          "no fixed item size means variable-size");
    check(contains(header, "tessera::ui::Node* virtual_3_ = nullptr;"),
          "the collection container is recorded");
    check(contains(header, "std::vector<Item_4> virtual_3_items_{};"),
          "the growable realized-window table");
    check(contains(header, "std::size_t virtual_3_count_ = 0;"), "the realized item count");
    check(contains(header, "std::size_t virtual_3_first_ = 0;"), "the realized window start");
    check(contains(header,
                   "std::unique_ptr<tessera::ui::Node> build_item_4(const std::string& item, "
                   "std::size_t tessera_index, std::size_t tessera_id_slot, Item_4& tessera_out);"),
          "the per-item builder signature");
    check(contains(header, "void apply_virtual_3(std::size_t tessera_first, "
                           "std::size_t tessera_last);"),
          "the window application signature");
    check(!contains(header, "void apply_list_4("),
          "a virtual template does not emit the eager list application");
    check(contains(header, "static constexpr double virtual_6_row_height_ = 40.0;"),
          "the grid row height is a generated constant");
    check(contains(header, "static constexpr int virtual_6_columns_ = 2;"),
          "the grid column count is a generated constant");
    check(contains(source, "void VirtualDemo::apply_virtual_3(std::size_t tessera_first, "
                           "std::size_t tessera_last)"),
          "the window application is emitted");
    check(contains(source, "tessera::ui::virtualization::ItemGeometry"),
          "the generated comment names the runtime geometry owner");
    check(contains(source, "const auto& tessera_source = state_items_;"),
          "the template reads the loop source list");
    check(!contains(source, "kMaxGeneratedVirtualItems"),
          "the realized window is not clamped to a fixed table");
    check(contains(source, "build_item_4(item, tessera_i, tessera_i, "
                           "tessera_next_items[tessera_next]);"),
          "a new item is built with its absolute index as the NodeId slot");
    check(contains(source, "virtual_3_->insert_child(tessera_next,"),
          "realized items attach to the collection container");
    check(contains(source, "apply_item_4(tessera_slot, tessera_source[tessera_i], tessera_i);"),
          "the per-item content pass runs over the window");
    check(contains(source, "apply_virtual_6("), "the grid window application is emitted");
    check(contains(source, "constexpr std::uint64_t kItemIdStride = 12ull;"),
          "item ids use a deterministic stride past the component's pre-order ids");
    check(contains(source, "set_element_kind(\"VirtualList\");"),
          "the collection node keeps its element kind");
    check(contains(source, "set_element_kind(\"VirtualGrid\");"), "the grid element kind");
    check(contains(source, "this->virtual_3_ = node_3.get();"),
          "build() records the collection container");
}

// The generated scroll entry point (virtualization-v1.md sections 4, 6 and 10): the component owns
// an ItemGeometry seeded from the emitted constants, refines it from realized item bounds, applies
// the overscanned window and dispatches the collection events.
void test_scroll_wiring_shape() {
    const auto result = codegen::generate(make_virtual_module());
    check(result.has_value(), "the scroll-wiring sample generates");
    if (!result) {
        std::cerr << "  error: " << first_error(result) << "\n";
        return;
    }
    const std::string& header = (*result)[0].text;
    const std::string& source = (*result)[1].text;
    check(contains(header, "#include \"tessera/ui/virtual/item_geometry.hpp\""),
          "the header includes the owned geometry");
    check(contains(header, "tessera::ui::virtualization::ItemGeometry virtual_3_geometry_{{"),
          "the collection owns a generated ItemGeometry member");
    check(contains(header, "kMaxGeneratedVirtualGeometryItems = 4096;"),
          "the generated geometry cap is emitted");
    check(contains(header, "kVirtualEndReachedThrottlePx = 200.0F;"),
          "the onEndReached throttle distance is emitted");
    check(contains(header, "static constexpr bool virtual_3_horizontal_ = false;"),
          "the main axis is a generated constant");
    check(contains(header, "bool scroll_virtual_3(float& tessera_scroll, "
                           "float tessera_viewport);"),
          "the scroll entry point declaration");
    check(contains(header, "bool virtual_3_end_armed_ = true;"),
          "the edge state of onEndReached is a generated member");
    check(contains(header, "std::uint64_t virtual_3_end_fires_ = 0;"),
          "the fire counter of onEndReached is a generated member");
    check(contains(header, "std::uint64_t virtual_3_end_throttled_ = 0;"),
          "the throttle counter of onEndReached is a generated member");
    check(contains(source, "bool VirtualDemo::scroll_virtual_3(float& tessera_scroll, "
                           "float tessera_viewport)"),
          "the scroll entry point is emitted");
    check(contains(source, "tessera_item->bounds().height;"),
          "a realized item reports its measured main-axis size through its bounds");
    check(contains(source, "virtual_3_geometry_.refine_measure(tessera_index, "
                           "tessera_measured);"),
          "the measure is forwarded to ItemGeometry::refine_measure");
    check(contains(source, "tessera::ui::virtualization::reanchor_scroll("),
          "a measure before the anchor re-anchors through the runtime rule");
    check(contains(source, "tessera_scroll = tessera_adjust.scroll_offset;"),
          "the caller's scroll offset is updated in place");
    check(contains(source, "visible_range(tessera_window_start, tessera_window_extent)"),
          "the realized window expands the viewport by the overscan");
    check(contains(source, "apply_virtual_3(tessera_range.first, tessera_range.last);"),
          "the window is applied through apply_virtual_<n>");
    check(contains(source, "tessera::ui::ScrollEventInfo tessera_scroll_info{};"),
          "onScroll receives the scene scroll payload");
    check(contains(source, "tessera_scroll_info.offset_y = tessera_scroll;"),
          "the payload carries the absolute scroll offset");
    check(contains(source,
                   "tessera_scroll_info.content_height = virtual_3_geometry_.total_extent();"),
          "the payload carries the geometry content extent");
    check(contains(source, "this->on_scroll(tessera_scroll_info);"),
          "the onScroll binding is invoked");
    check(contains(source, "this->handler_on_end_reached(tessera_end_event);"),
          "the onEndReached handler is invoked");
    check(contains(source, "tessera_end_distance < kVirtualEndReachedThrottlePx)"),
          "the distance throttle gates onEndReached");
    check(contains(source, "apply_virtual_6(tessera_first, tessera_last);"),
          "the grid applies a whole-row item window");
    check(contains(source, "tessera_item_index / tessera_columns"),
          "a grid cell measure maps to its row");
    check(contains(source, "// [DEVELOPER_LOGIC_START: bind::on_scroll]"),
          "the onScroll binding keeps its protected region");
    // No scene adapter is generated for a collection-level event (there is no scroll EventType).
    check(!contains(source, "event_adapter_3_onScroll"),
          "collection events are not scene event adapters");
}

void test_goldens(bool update) {
    const auto result = codegen::generate(make_virtual_module());
    check(result.has_value(), "the virtual golden sample generates");
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
        check_eq(golden, file.text, file.path + " golden matches (codegen_virtual_tests --write)");
    }
    if (update) {
        std::cout << "virtual codegen goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = codegen::generate(make_virtual_module());
    const auto second = codegen::generate(make_virtual_module());
    check(first.has_value() && second.has_value(), "the virtual sample generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second), "two runs are byte-identical");
    }
}

void test_rejections() {
    const auto expect_error = [](const ir::IrModule& module, std::string_view what) {
        const auto result = codegen::generate(module);
        check(!result.has_value(), what);
        check(first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              std::string(what) + " reports a TESSERA_CODEGEN_ code");
    };
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].item_template.reset();
        expect_error(module, "a virtual collection without an itemTemplate is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].item_template->loop.reset();
        expect_error(module, "an itemTemplate without the loop is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].props["estimateSize"] = ir::make_ref("state::count");
        expect_error(module, "a dynamic estimateSize is rejected in this slice");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].props["gap"] = ir::make_ref("state::count");
        expect_error(module, "a dynamic gap is rejected in this slice");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].props["orientation"] =
            ir::make_literal(std::string("diagonal"));
        expect_error(module, "an unsupported orientation is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].item_template->key.reset();
        expect_error(module, "an item template without a key is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].item_template->loop->source =
            ir::make_literal(std::string("nope"));
        expect_error(module, "a non-reference item source is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].condition = ir::make_literal(true);
        expect_error(module, "a conditional virtual collection is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].loop =
            ir::Loop{"item", "", ir::make_ref("state::items")};
        expect_error(module, "a looped virtual collection is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].state[1].type = "list<Row>";
        expect_error(module, "a record element type of a virtual source is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].item_template->condition = ir::make_literal(true);
        expect_error(module, "a conditional item template is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        module.components[0].root.children[1].item_template->children.emplace_back(
            leaf(ir::NodeKind::TextInput, "node::30", "30"));
        expect_error(module, "a TextInput inside a virtual item template is deferred");
    }
    // Collection-level event contracts (virtualization-v1.md section 10).
    const auto add_handler = [](ir::IrModule& module, std::string id, std::string payload) {
        ir::Handler handler;
        handler.id = std::move(id);
        if (!payload.empty()) {
            handler.params.push_back(ir::Param{"e", std::move(payload)});
        }
        handler.source = span(200, 10, 12, 3);
        const std::string name = handler.id;
        module.components[0].handlers.push_back(std::move(handler));
        return name;
    };
    {
        ir::IrModule module = make_virtual_module();
        const std::string handler = add_handler(module, "handler::scroll_click", "ClickEvent");
        module.components[0].root.children[1].events["onScroll"] = handler;
        expect_error(module, "an onScroll payload other than ScrollEvent is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        const std::string handler = add_handler(module, "handler::end_payload", "ScrollEvent");
        module.components[0].root.children[1].events["onEndReached"] = handler;
        expect_error(module, "an onEndReached handler with a parameter is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        ir::Binding end;
        end.id = "bind::on_grid_end";
        end.component = "comp::VirtualDemo";
        end.kind = ir::BindingKind::Event;
        end.region = "on_grid_end";
        end.source = span(210, 10, 13, 3);
        end.params.push_back({"e", "ScrollEvent"});
        module.bindings.push_back(std::move(end));
        module.components[0].root.children[1].events["onEndReached"] = "bind::on_grid_end";
        expect_error(module, "an onEndReached binding with a payload is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        for (ir::Binding& binding : module.bindings) {
            if (binding.id == "bind::on_scroll") {
                binding.return_type = "int";
            }
        }
        expect_error(module, "an onScroll binding with a return value is rejected");
    }
    {
        ir::IrModule module = make_virtual_module();
        ir::Node& grid = module.components[0].root.children[2];
        grid.props.erase("columns");
        const std::string handler = add_handler(module, "handler::grid_end", "");
        grid.events["onEndReached"] = handler;
        expect_error(module, "a grid collection event without a column count is rejected");
    }
}

void test_merger_idempotency() {
    const auto result = codegen::generate(make_virtual_module());
    check(result.has_value(), "the virtual sample generates for the merger test");
    if (!result) {
        return;
    }
    std::string generated;
    for (const codegen::GeneratedFile& file : *result) {
        if (file.path == "VirtualDemo.cpp") {
            generated = file.text;
        }
    }
    check(!generated.empty(), "the generated virtual source exists");
    constexpr std::string_view kMarker = "// [DEVELOPER_LOGIC_START: bind::on_click]";
    const std::size_t marker = generated.find(kMarker);
    check(marker != std::string::npos, "the on_click region survives next to the collections");
    if (marker == std::string::npos) {
        return;
    }
    std::string existing = generated;
    existing.insert(marker + kMarker.size(), "\n    // developer body next to generated windows");
    constexpr std::string_view kScrollMarker = "// [DEVELOPER_LOGIC_START: bind::on_scroll]";
    const std::size_t scroll_marker = existing.find(kScrollMarker);
    check(scroll_marker != std::string::npos, "the on_scroll region exists");
    if (scroll_marker == std::string::npos) {
        return;
    }
    existing.insert(scroll_marker + kScrollMarker.size(), "\n    // developer scroll observation");
    const tessera::compiler::MergeResult first = tessera::compiler::merge(generated, existing);
    check(first.text.find("developer body next to generated windows") != std::string::npos,
          "the developer body survives the merge");
    check(first.text.find("developer scroll observation") != std::string::npos,
          "the onScroll developer body survives the merge");
    const tessera::compiler::MergeResult second = tessera::compiler::merge(first.text, existing);
    check_eq(second.text, first.text, "merge(merge(g, e), e) == merge(g, e) over virtual output");
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
    test_scroll_wiring_shape();
    test_goldens(false);
    test_determinism();
    test_rejections();
    test_merger_idempotency();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "virtual codegen tests passed\n";
    return 0;
}
