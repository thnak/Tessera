// SPDX-License-Identifier: MIT

// `Path` -> `PaintKind::Vector` (V1-M2; docs/design/compiler.md section 11).
// `PaintVectorStyle::asset` is an embedded-asset name while the intrinsic carries inline
// `d`/`viewBox`, so codegen synthesizes one SVG document per `Path` node from the literal props,
// embeds the exact bytes as a generated asset bundle, and emits a vector payload naming that asset.
// The existing asset -> SVG decode -> path rasterizer chain paints it; no new paint payload and no
// new RHI path.
//
// This test pins:
// - the emitted shape: the `PaintData` lines (kind + asset name), the accessible name from `label`,
//   and the synthesized-asset registration in `<module>_vector_assets.hpp`;
// - the exact SVG bytes: fixed attribute order, spec defaults (`fill`/`stroke` `"none"`,
//   `strokeWidth` `1`), explicit values, and byte-identical regeneration;
// - loud rejection of every non-literal prop and of a malformed `viewBox` / `var()` color;
// - the committed golden under tests/golden/codegen/vector/ and, when it is compiled by the build,
//   the runtime payload: kind, asset name and resolution through the generated bundle + SVG decode.
//
// Deliberate golden update path: `codegen_vector_payload_tests --write` rewrites
// tests/golden/codegen/vector/VectorIcon.* and vicon_vector_assets.hpp.

#include "codegen_vector_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#ifdef TESSERA_VECTOR_SAMPLE
#include "VectorIcon.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
#include "tessera/ui/scene/node.hpp"
#include "vicon_vector_assets.hpp"
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

using tessera::test::codegen_vector::make_vector_module;

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

const codegen::GeneratedFile* find_file(const std::vector<codegen::GeneratedFile>& files,
                                        std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

// The exact document the fixture's explicit `Path` must synthesize.
constexpr std::string_view kExplicitSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<path d=\"M4 4 L20 4 L20 20 Z\" fill=\"#2e8b57\" stroke=\"#102030\" stroke-width=\"2\"/>"
    "</svg>";

// The same for the defaults-only `Path` (`fill`/`stroke` "none", `stroke-width` 1).
constexpr std::string_view kDefaultSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
    "<path d=\"M2 12 L12 2 L22 12\" fill=\"none\" stroke=\"none\" stroke-width=\"1\"/>"
    "</svg>";

// The decoded `kAssetData` of a generated vector-assets header.
std::vector<std::uint8_t> asset_data(std::string_view header) {
    std::vector<std::uint8_t> bytes;
    const std::size_t begin = header.find("kAssetData[] = {");
    if (begin == std::string_view::npos) {
        return bytes;
    }
    const std::size_t end = header.find("};", begin);
    if (end == std::string_view::npos) {
        return bytes;
    }
    const std::string_view block = header.substr(begin, end - begin);
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i + 3 < block.size(); ++i) {
        if (block[i] != '0' || block[i + 1] != 'x') {
            continue;
        }
        const int high = digit(block[i + 2]);
        const int low = digit(block[i + 3]);
        if (high < 0 || low < 0) {
            continue;
        }
        bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
        i += 3;
    }
    return bytes;
}

// One `AssetEntry` row of a generated header, read back from the table text.
struct EntryRow {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    bool found = false;
};

EntryRow entry_row(std::string_view header, std::string_view name) {
    EntryRow row;
    const std::string marker = "\"" + std::string(name) + "\", ";
    const std::size_t begin = header.find(marker);
    if (begin == std::string_view::npos) {
        return row;
    }
    std::size_t index = begin + marker.size();
    const auto parse_uint = [&](std::uint32_t& out) {
        std::uint64_t value = 0;
        std::size_t digits = 0;
        while (index < header.size() && header[index] >= '0' && header[index] <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(header[index] - '0');
            ++index;
            ++digits;
        }
        out = static_cast<std::uint32_t>(value);
        return digits != 0;
    };
    if (!parse_uint(row.offset) || index >= header.size() || header[index] != ',') {
        return row;
    }
    ++index; // ','
    if (index < header.size() && header[index] == ' ') {
        ++index;
    }
    if (!parse_uint(row.size)) {
        return row;
    }
    row.found = true;
    return row;
}

// The SVG bytes of one asset, sliced out of the concatenated blob by its entry.
std::string asset_bytes(std::string_view header, std::string_view name) {
    const EntryRow row = entry_row(header, name);
    const std::vector<std::uint8_t> data = asset_data(header);
    if (!row.found || row.offset > data.size() || row.size > data.size() - row.offset) {
        return {};
    }
    std::string out;
    out.reserve(row.size);
    for (std::uint32_t i = 0; i < row.size; ++i) {
        out.push_back(static_cast<char>(data[row.offset + i]));
    }
    return out;
}

// The fixture's first `Path` node (the one that writes every prop).
ir::Node& explicit_node(ir::IrModule& module) {
    return module.components[0].root.children[0];
}

void test_fixture_is_valid() {
    const ir::IrModule module = make_vector_module();
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the `Path` fixture is a valid IR module");
}

void test_emitted_payload_lines() {
    const auto result = codegen::generate(make_vector_module());
    check(result.has_value(), "the `Path` fixture generates: " + first_error(result));
    if (!result) {
        return;
    }
    const codegen::GeneratedFile* source = find_file(*result, "VectorIcon.cpp");
    check(source != nullptr, "the component source is emitted");
    if (source == nullptr) {
        return;
    }
    // The vector payload, not a text payload: kind Vector plus the synthesized asset name.
    for (const std::string_view snippet :
         {"tessera::ui::PaintData paint_node_2;",
          "paint_node_2.kind = tessera::ui::PaintKind::Vector;",
          "paint_node_2.vector.asset = \"vector/VectorIcon/2.svg\";",
          "node_2->set_paint_data(std::move(paint_node_2));",
          "paint_node_3.kind = tessera::ui::PaintKind::Vector;",
          "paint_node_3.vector.asset = \"vector/VectorIcon/3.svg\";"}) {
        check(contains(source->text, snippet),
              std::string("the emitted payload contains: ") + std::string(snippet));
    }
    check(contains(source->text, "node_2->set_aria(\"aria-label\", \"Logo\");"),
          "the `label` prop becomes the accessible name");
    check(!contains(source->text, "node_3->set_aria(\"aria-label\""),
          "a `Path` without a label emits no accessible name");

    // The registration: one extra module-level file with the embedded bundle.
    const codegen::GeneratedFile* assets = find_file(*result, "vicon_vector_assets.hpp");
    check(assets != nullptr, "the synthesized-asset header is emitted");
    if (assets == nullptr) {
        return;
    }
    for (const std::string_view snippet :
         {"inline constexpr std::size_t kAssetCount = 2;", "{\"vector/VectorIcon/2.svg\", 0, ",
          "{\"vector/VectorIcon/3.svg\", ", "\"image/svg+xml\"",
          "[[nodiscard]] inline ::tessera::ui::assets::AssetBundle bundle() noexcept {",
          "std::span<const ::tessera::ui::assets::AssetEntry>(kAssetEntries, kAssetCount)"}) {
        check(contains(assets->text, snippet),
              std::string("the asset header contains: ") + std::string(snippet));
    }
}

void test_svg_bytes_are_exact() {
    const auto result = codegen::generate(make_vector_module());
    check(result.has_value(), "the `Path` fixture generates for the byte check");
    if (!result) {
        return;
    }
    const codegen::GeneratedFile* assets = find_file(*result, "vicon_vector_assets.hpp");
    check(assets != nullptr, "the asset header is present for the byte check");
    if (assets == nullptr) {
        return;
    }
    check_eq(asset_bytes(assets->text, "vector/VectorIcon/2.svg"), std::string(kExplicitSvg),
             "the explicit `Path` synthesizes the pinned document byte-for-byte");
    check_eq(asset_bytes(assets->text, "vector/VectorIcon/3.svg"), std::string(kDefaultSvg),
             "the defaults-only `Path` synthesizes the spec defaults byte-for-byte");
    // The concatenated blob is exactly the two documents in sorted name order.
    std::string blob;
    for (const std::uint8_t byte : asset_data(assets->text)) {
        blob.push_back(static_cast<char>(byte));
    }
    check_eq(blob, std::string(kExplicitSvg) + std::string(kDefaultSvg),
             "the bundle data blob is the two documents in sorted name order");
}

void test_regeneration_is_byte_identical() {
    const auto first = codegen::generate(make_vector_module());
    const auto second = codegen::generate(make_vector_module());
    check(first.has_value() && second.has_value(), "the fixture generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second),
                 "two runs of the same source are byte-identical");
    }
}

void test_goldens(bool update) {
    const auto result = codegen::generate(make_vector_module());
    check(result.has_value(), "the `Path` golden sample generates");
    if (!result) {
        return;
    }
    const std::string directory = std::string(TESSERA_CODEGEN_GOLDEN_DIR) + "/vector";
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
                 file.path + " golden matches (codegen_vector_payload_tests --write)");
    }
    if (update) {
        std::cout << "vector payload codegen goldens written to " << directory << "\n";
    }
}

void test_rejections() {
    const auto expect_error = [](const ir::IrModule& module, std::string_view what,
                                 std::string_view needle) {
        const auto result = codegen::generate(module);
        check(!result.has_value(), what);
        const std::string error = first_error(result);
        check(error.rfind("TESSERA_CODEGEN_UNSUPPORTED", 0) == 0,
              std::string(what) + " reports TESSERA_CODEGEN_UNSUPPORTED");
        check(contains(error, needle),
              std::string(what) + " names the offending prop: " + std::string(needle));
    };
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["d"] = ir::make_ref("prop::d");
        expect_error(module, "a non-literal `d` is rejected", "`d`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["viewBox"] = ir::make_ref("prop::box");
        expect_error(module, "a non-literal `viewBox` is rejected", "`viewBox`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["fill"] = ir::make_ref("state::accent");
        expect_error(module, "a non-literal `fill` is rejected", "`fill`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["stroke"] = ir::make_ref("state::edge");
        expect_error(module, "a non-literal `stroke` is rejected", "`stroke`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["strokeWidth"] = ir::make_ref("prop::width");
        expect_error(module, "a non-literal `strokeWidth` is rejected", "`strokeWidth`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["label"] = ir::make_ref("prop::label");
        expect_error(module, "a non-literal `label` is rejected", "`label`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["viewBox"] = ir::make_literal(std::string("0 0 24"));
        expect_error(module, "a three-number `viewBox` is rejected", "`viewBox`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["viewBox"] = ir::make_literal(std::string("0 0 0 24"));
        expect_error(module, "a zero-width `viewBox` is rejected", "`viewBox`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["fill"] = ir::make_literal(std::string("var(--accent)"));
        expect_error(module, "a `var()` color is rejected", "`fill`");
    }
    {
        ir::IrModule module = make_vector_module();
        explicit_node(module).props["d"] = ir::make_literal(std::string(""));
        expect_error(module, "an empty `d` is rejected", "`d`");
    }
}

#ifdef TESSERA_VECTOR_SAMPLE

namespace ui = tessera::ui;

void test_runtime_payload() {
    ui::Node scene(1000);
    vicon::gen::VectorIcon component;
    component.build(scene);
    check(scene.children().size() == 1, "the component builds one root child");
    if (scene.children().empty()) {
        return;
    }
    ui::Node& root = *scene.children()[0];
    check(root.children().size() == 2, "the root holds the two `Path` nodes");
    if (root.children().size() != 2) {
        return;
    }

    // 1. The payload reached the node: kind Vector and the synthesized asset name.
    ui::Node& explicit_view = *root.children()[0];
    check(explicit_view.element_kind() == "Path", "the explicit node is a `Path`");
    const auto& data = explicit_view.paint_data();
    check(data.has_value(), "the explicit `Path` carries a paint payload");
    if (data.has_value()) {
        check(data->kind == ui::PaintKind::Vector, "the payload kind is Vector");
        check_eq(std::string(data->vector.asset), "vector/VectorIcon/2.svg",
                 "the payload names the synthesized asset");
    }
    bool labelled = false;
    for (const ui::Node::AriaAttribute& attribute : explicit_view.aria_attributes()) {
        if (attribute.name == "aria-label") {
            labelled = attribute.value == "Logo";
        }
    }
    check(labelled, "the `label` prop is the node's accessible name");

    ui::Node& default_view = *root.children()[1];
    const auto& default_data = default_view.paint_data();
    check(default_data.has_value() && default_data->kind == ui::PaintKind::Vector,
          "the defaults-only `Path` carries a Vector payload");
    if (default_data.has_value()) {
        check_eq(std::string(default_data->vector.asset), "vector/VectorIcon/3.svg",
                 "the defaults-only payload names its own asset");
    }

    // 2. The asset resolves through the generated bundle, with the exact document bytes.
    const auto bundle = vicon::gen::vector_assets::bundle();
    const auto asset = bundle.find("vector/VectorIcon/2.svg");
    check(asset.has_value(), "the synthesized asset resolves through the bundle");
    if (asset.has_value()) {
        check_eq(
            std::string(reinterpret_cast<const char*>(asset->bytes.data()), asset->bytes.size()),
            std::string(kExplicitSvg), "the bundled bytes are the synthesized document");
        check_eq(std::string(asset->mime), std::string("image/svg+xml"), "the MIME hint is SVG");
    }
    check(bundle.size() == 2, "the bundle holds one asset per `Path` node");

    // 3. The document decodes through the existing SVG loader (the paint chain's first step).
    if (tessera::rendering::path::svg_backend_available()) {
        tessera::rendering::path::SvgDocumentCache cache;
        const auto document =
            tessera::ui::assets::load_asset_svg(bundle, "vector/VectorIcon/2.svg", cache);
        check(document.has_value(), "the synthesized SVG decodes through load_asset_svg");
        if (document.has_value()) {
            check((*document)->shapes.size() == 1, "the document holds one path shape");
            check((*document)->width == 24 && (*document)->height == 24,
                  "the decoded document size is the viewBox size");
        }
    } else {
        std::cout << "vector decode check skipped: no SVG backend in this build\n";
    }
}

#endif // TESSERA_VECTOR_SAMPLE

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    if (update) {
        test_goldens(true);
        return failures == 0 ? 0 : 1;
    }
    test_fixture_is_valid();
    test_emitted_payload_lines();
    test_svg_bytes_are_exact();
    test_regeneration_is_byte_identical();
    test_goldens(false);
    test_rejections();
#ifdef TESSERA_VECTOR_SAMPLE
    test_runtime_payload();
#endif
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vector payload codegen tests passed\n";
    return 0;
}
