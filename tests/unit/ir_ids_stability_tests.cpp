// SPDX-License-Identifier: MIT
//
// Stable component identity (docs/specs/automation-ids-v1.md section 2.1, issue #70 leftovers):
// a component's id is a function of its source path, its name and its position among same-named
// components of that file, never of its absolute index in the module. Inserting or reordering a
// differently named component must leave every existing id (and every generated file of the
// untouched components) byte-identical; reordering two same-named components intentionally swaps
// their ids. The last test proves the golden consequence on the real `Field`/`Form` library TSX:
// inserting a component before them keeps their generated C++ byte-identical, and the committed
// library data still agrees with a fresh lowering.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/lowering/library_components.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_LIBRARY_TSX_DIR
#error "TESSERA_LIBRARY_TSX_DIR must be defined by the build"
#endif

namespace {

namespace fs = std::filesystem;
namespace ir = tessera::compiler::ir;
namespace ids = tessera::compiler::ids;
namespace codegen = tessera::compiler::codegen;
namespace lowering = tessera::compiler::lowering;

using jsx_test::check;
using jsx_test::check_eq;
using lowering_module_test::FileFixture;
using lowering_module_test::first_message;
using lowering_module_test::parse_file;

constexpr std::string_view kScreen = "ui/Screen.tsx";
constexpr std::string_view kCards = "ui/Cards.tsx";
constexpr std::string_view kField = "packages/tessera-ui/Field.tsx";
constexpr std::string_view kForm = "packages/tessera-ui/Form.tsx";
constexpr std::string_view kAaa = "packages/tessera-ui/Aaa.tsx";

ir::Node make_node(std::string id, ir::NodeKind kind) {
    ir::Node node;
    node.id = std::move(id);
    node.kind = kind;
    return node;
}

struct Spec {
    const char* name;
    const char* file;
};

// Every component is a `View` with one `Text` child, so the component id, the root uiId and a
// child uiId are all observed.
ir::IrModule make_module(std::initializer_list<Spec> specs) {
    ir::IrModule module;
    int next = 1;
    for (const Spec& spec : specs) {
        ir::Component component;
        component.id = std::string("comp::") + spec.name;
        component.name = spec.name;
        component.source.file = spec.file;
        component.root = make_node("node::" + std::to_string(next++), ir::NodeKind::View);
        component.root.children.push_back(
            make_node("node::" + std::to_string(next++), ir::NodeKind::Text));
        module.components.push_back(std::move(component));
    }
    return module;
}

struct Ids {
    std::string component;
    std::string root;
    std::string child;
};

Ids ids_of(const ir::Component& component) {
    Ids out;
    out.component = component.component_id;
    out.root = component.root.ui_id;
    if (!component.root.children.empty()) {
        out.child = component.root.children.front().ui_id;
    }
    return out;
}

std::string describe(const Ids& value) {
    return value.component + " / " + value.root + " / " + value.child;
}

bool same(const Ids& left, const Ids& right) {
    return left.component == right.component && left.root == right.root &&
           left.child == right.child;
}

void check_component_ids(const ir::IrModule& module, std::size_t index, const Ids& expected,
                         std::string_view what) {
    const Ids actual = ids_of(module.components[index]);
    check(same(actual, expected), std::string(what) + " (expected " + describe(expected) +
                                      ", got " + describe(actual) + ")");
}

void test_inserting_a_component_keeps_the_others() {
    ir::IrModule base =
        make_module({{"A", kScreen.data()}, {"B", kScreen.data()}, {"C", kScreen.data()}});
    check(ids::assign_identity(base, "factory").has_value(), "the base module assigns");
    const Ids a = ids_of(base.components[0]);
    const Ids b = ids_of(base.components[1]);
    const Ids c = ids_of(base.components[2]);
    check(!a.component.empty() && !b.component.empty() && !c.component.empty(),
          "the base components got ids");

    // X before A (same file, first declaration).
    ir::IrModule before = make_module({{"X", kScreen.data()},
                                       {"A", kScreen.data()},
                                       {"B", kScreen.data()},
                                       {"C", kScreen.data()}});
    check(ids::assign_identity(before, "factory").has_value(),
          "the insertion-before module assigns");
    const Ids x_before = ids_of(before.components[0]);
    check(!x_before.component.empty(), "X got an id");
    check(x_before.component != a.component && x_before.component != b.component &&
              x_before.component != c.component,
          "X's component id is fresh");
    check_component_ids(before, 1, a, "A keeps its id when X is inserted before it");
    check_component_ids(before, 2, b, "B keeps its id when X is inserted before A");
    check_component_ids(before, 3, c, "C keeps its id when X is inserted before A");

    // X between A and B.
    ir::IrModule between = make_module({{"A", kScreen.data()},
                                        {"X", kScreen.data()},
                                        {"B", kScreen.data()},
                                        {"C", kScreen.data()}});
    check(ids::assign_identity(between, "factory").has_value(),
          "the insertion-between module assigns");
    const Ids x_between = ids_of(between.components[1]);
    check(!x_between.component.empty() && same(x_between, x_before),
          "X's id depends on its name, not on where it is inserted");
    check_component_ids(between, 0, a, "A keeps its id when X is inserted after it");
    check_component_ids(between, 2, b, "B keeps its id when X is inserted before it");
    check_component_ids(between, 3, c, "C keeps its id when X is inserted between A and B");

    // Deterministic: the same insertion produces the same ids.
    ir::IrModule again = make_module({{"A", kScreen.data()},
                                      {"X", kScreen.data()},
                                      {"B", kScreen.data()},
                                      {"C", kScreen.data()}});
    check(ids::assign_identity(again, "factory").has_value(), "a repeat insertion assigns");
    check(same(ids_of(again.components[1]), x_between),
          "the inserted component id is deterministic");
}

void test_same_named_components() {
    // Same name in the same file: the occurrence disambiguates them. Node paths get the same
    // `#occurrence` suffix so their ids differ too.
    ir::IrModule pair = make_module({{"Card", kCards.data()}, {"Card", kCards.data()}});
    check(ids::assign_identity(pair, "factory").has_value(), "the same-named pair assigns");
    const Ids first = ids_of(pair.components[0]);
    const Ids second = ids_of(pair.components[1]);
    check(first.component != second.component, "same-named components get distinct component ids");
    check(first.root != second.root, "same-named components get distinct root uiIds");
    check(first.child != second.child, "same-named components get distinct child uiIds");

    // Inserting a differently named component before them changes neither.
    ir::IrModule with_x =
        make_module({{"X", "ui/Extra.tsx"}, {"Card", kCards.data()}, {"Card", kCards.data()}});
    check(ids::assign_identity(with_x, "factory").has_value(), "the insertion module assigns");
    check_component_ids(with_x, 1, first,
                        "the first Card keeps its id when X is inserted before it");
    check_component_ids(with_x, 2, second,
                        "the second Card keeps its id when X is inserted before it");

    // The manifest lists both with their occurrence-qualified paths.
    const std::string manifest = ids::write_manifest(pair, "factory");
    check(manifest.find("\"path\": \"Card/root[0]\"") != std::string::npos,
          "the first occurrence keeps the bare component path prefix");
    check(manifest.find("\"path\": \"Card#1/root[0]\"") != std::string::npos,
          "the second occurrence gets the #1 path prefix");
}

void test_reordering_same_named_components_swaps_ids() {
    ir::IrModule ordered = make_module({{"Card", kCards.data()}, {"Card", kCards.data()}});
    // Distinct debug ids name the two declarations, so the test can show which one moved.
    ordered.components[0].id = "comp::CardFirst";
    ordered.components[1].id = "comp::CardSecond";
    check(ids::assign_identity(ordered, "factory").has_value(), "the ordered pair assigns");
    const Ids first = ids_of(ordered.components[0]);
    const Ids second = ids_of(ordered.components[1]);
    check(!same(first, second), "the two occurrences start distinct");

    ir::IrModule swapped = make_module({{"Card", kCards.data()}, {"Card", kCards.data()}});
    swapped.components[0].id = "comp::CardFirst";
    swapped.components[1].id = "comp::CardSecond";
    std::swap(swapped.components[0], swapped.components[1]);
    check(ids::assign_identity(swapped, "factory").has_value(), "the swapped pair assigns");
    // Identity is positional among same-named declarations: the component that now sits first
    // takes the first occurrence's id, so the two declarations exchange ids.
    check_eq(swapped.components[0].id, "comp::CardSecond", "the second declaration now sits first");
    check_component_ids(
        swapped, 0, first,
        "the declaration moved to the first position takes the first occurrence's ids");
    check_eq(swapped.components[1].id, "comp::CardFirst", "the first declaration now sits second");
    check_component_ids(
        swapped, 1, second,
        "the declaration moved to the second position takes the second occurrence's ids");
}

void test_duplicate_detection_is_still_loud() {
    // Two components cannot silently share a componentId: IR validation reports the duplicate.
    ir::IrModule module = make_module({{"A", kScreen.data()}, {"B", kScreen.data()}});
    check(ids::assign_identity(module, "factory").has_value(),
          "the duplicate-check module assigns");
    module.components[1].component_id = module.components[0].component_id;
    bool duplicate_reported = false;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        if (diagnostic.code == "TESSERA_IR_DUPLICATE_ID" &&
            diagnostic.message.find("componentId") != std::string::npos) {
            duplicate_reported = true;
        }
    }
    check(duplicate_reported, "a duplicated componentId is TESSERA_IR_DUPLICATE_ID");

    // A reused explicit node automationId is still an assignment error, not a silent overwrite.
    ir::IrModule explicit_ids = make_module({{"A", kScreen.data()}, {"B", kScreen.data()}});
    explicit_ids.components[0].root.automation_id = "7";
    explicit_ids.components[1].root.automation_id = "7";
    const auto result = ids::assign_identity(explicit_ids, "factory");
    check(!result.has_value() &&
              result.error().message.find("duplicate automationId") != std::string::npos,
          "a duplicate explicit automationId is diagnosed");
    check(explicit_ids.components[0].component_id.empty(),
          "a failed assignment leaves the module unchanged");
}

// The language has no explicit component id prop: `id` is an element prop on a node, and IR v1
// `Component` has no `automationId`/`componentId` override field. Component identity is derived
// only, so the explicit-id case of the task does not apply (documented in
// docs/specs/automation-ids-v1.md section 2.1).

std::string library_source_path(std::string_view file) {
    return (fs::path(TESSERA_LIBRARY_TSX_DIR) / fs::path(file).filename()).string();
}

std::unique_ptr<FileFixture> parse_library_file(std::string_view file) {
    const std::string path = library_source_path(file);
    std::ifstream in(path, std::ios::binary);
    check(in.good(), "the library TSX source exists: " + path);
    if (!in) {
        return nullptr;
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // The project-relative path, not the build path: it is what the IR records as `source.file`
    // and therefore part of the stamped id.
    return parse_file(std::string(file), std::move(text));
}

ir::IrModule make_library_module(bool with_inserted_component) {
    ir::IrModule module;
    module.name = "demo";
    const auto append = [&](std::unique_ptr<FileFixture> fixture) {
        if (!fixture) {
            return false;
        }
        auto lowered = lowering::lower_module(fixture->module_source(), "demo");
        check(lowered.has_value(), "the library source lowers: " + first_message(lowered));
        if (!lowered) {
            return false;
        }
        for (ir::Component& component : lowered->components) {
            module.components.push_back(std::move(component));
        }
        return true;
    };
    if (with_inserted_component) {
        // A component declared before Field/Form in the module: the insertion case.
        constexpr std::string_view kAaaSource = "export function Aaa(): Node {\n"
                                                "  return <View className=\"aaa\" />;\n"
                                                "}\n";
        append(parse_file(std::string(kAaa), std::string(kAaaSource)));
    }
    append(parse_library_file(kField));
    append(parse_library_file(kForm));
    check(module.components.size() == (with_inserted_component ? 3U : 2U),
          "the library module carries the expected components");
    return module;
}

std::string file_text(const std::vector<codegen::GeneratedFile>& files, std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

void test_library_goldens_survive_insertion() {
    ir::IrModule before = make_library_module(false);
    check(ids::assign_library_identity(before).has_value(), "the library identity stamps");
    ir::IrModule after = make_library_module(true);
    check(ids::assign_library_identity(after).has_value(), "the library identity stamps with Aaa");

    const auto before_files = codegen::generate(before);
    const auto after_files = codegen::generate(after);
    check(before_files.has_value(), "the library module generates");
    check(after_files.has_value(), "the library module with Aaa generates");
    if (!before_files || !after_files) {
        return;
    }
    for (const std::string_view path : {"Field.hpp", "Field.cpp", "Form.hpp", "Form.cpp"}) {
        check(!file_text(*before_files, path).empty(),
              "the untouched component generates " + std::string(path));
        check_eq(file_text(*after_files, path), file_text(*before_files, path),
                 "inserting Aaa before Field/Form keeps " + std::string(path) + " byte-identical");
    }
    check(!file_text(*after_files, "Aaa.cpp").empty(),
          "the inserted component generates its own file");
    check(file_text(*before_files, "Aaa.cpp").empty(),
          "the inserted component is absent before the insertion");

    // The committed library data (compiled into tessera_lowering) must carry exactly the ids a
    // fresh lowering + `kLibraryIdSalt` produces; this is what keeps the committed goldens current.
    for (const std::string_view name : {"Field", "Form"}) {
        const lowering::LibraryComponent* library = lowering::find_library_component(name);
        check(library != nullptr, std::string(name) + " is a declared library component");
        if (library == nullptr) {
            continue;
        }
        ir::IrModule committed;
        committed.name = "demo";
        committed.components.push_back(lowering::make_library_component(*library));
        const auto generated = codegen::generate(committed);
        check(generated.has_value(), std::string(name) + " generates from committed data");
        if (!generated) {
            continue;
        }
        const auto lowered = codegen::generate(before);
        check(lowered.has_value(), "the lowered library module generates");
        if (lowered) {
            check_eq(file_text(*generated, std::string(name) + ".cpp"),
                     file_text(*lowered, std::string(name) + ".cpp"),
                     std::string(name) +
                         ": the committed library IR and a fresh lowering generate the same C++");
        }
    }
}

} // namespace

int main() {
    test_inserting_a_component_keeps_the_others();
    test_same_named_components();
    test_reordering_same_named_components_swaps_ids();
    test_duplicate_detection_is_still_loud();
    test_library_goldens_survive_insertion();
    return jsx_test::finish("ir ids stability tests");
}
