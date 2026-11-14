// SPDX-License-Identifier: MIT

// `packages/tessera-ui/SafeArea.tsx` (issue #223) lowers, gets identity under a salt, validates and
// generates C++: a role-less wrapper whose class tokens carry the props, and a demo
// that imports it as `<SafeArea>` so the runtime test can drive it through the app host.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif
#ifndef TESSERA_SAFE_AREA_TSX
#error "TESSERA_SAFE_AREA_TSX must be defined by the build"
#endif

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace ids = tessera::compiler::ids;

bool write_goldens = false;

// Two SafeAreas: the defaults, and one with explicit edges, a floor and margin mode.
constexpr const char* kDemo =
    "import { SafeArea } from \"./SafeArea\";\n"
    "export function SafeAreaDemo() {\n"
    "  return (\n"
    "    <View className=\"fill\">\n"
    "      <SafeArea>\n"
    "        <View className=\"fill\" />\n"
    "      </SafeArea>\n"
    "    </View>\n"
    "  );\n"
    "}\n"
    "export function SafeAreaOptions() {\n"
    "  return (\n"
    "    <View className=\"fill\">\n"
    "      <SafeArea edges=\"top+bottom\" sources=\"system-bars+gestures\" minimum={8} "
    "mode=\"margin\">\n"
    "        <View className=\"fill\" />\n"
    "      </SafeArea>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

void check_goldens(const std::vector<codegen::GeneratedFile>& files) {
    for (const codegen::GeneratedFile& file : files) {
        const std::filesystem::path path =
            std::filesystem::path(TESSERA_CODEGEN_GOLDEN_DIR) / file.path;
        if (write_goldens) {
            std::ofstream out(path, std::ios::binary);
            out << file.text;
            continue;
        }
        std::ifstream golden(path, std::ios::binary);
        std::ostringstream text;
        text << golden.rdbuf();
        check(text.str() == file.text, "the golden " + file.path + " is current");
    }
}

void test_demo_project(const std::string& source) {
    Project project;
    project.add(parse_file("ui/SafeArea.tsx", source));
    project.add(parse_file("ui/SafeAreaDemo.tsx", kDemo));
    auto result = project.lower("safe_area_demo");
    check(result.has_value(), "the demo project lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* demo = find_component(*result, "SafeAreaDemo");
    check(demo != nullptr && demo->root.children.size() == 1 &&
              demo->root.children[0].kind == ir::NodeKind::Component &&
              demo->root.children[0].component == "comp::SafeArea",
          "`<SafeArea>` is the library component, not the intrinsic");
    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    check(ids::assign_identity(module, "safe-area-demo").has_value(), "identity assigns");
    const auto diagnostics = ir::validate(module);
    check(diagnostics.empty(), "the demo module validates: " +
                                   (diagnostics.empty() ? std::string() : diagnostics[0].message));
    const auto generated = codegen::generate(module);
    check(generated.has_value(), "the demo generates C++");
    if (generated) {
        check_goldens(*generated);
    }
}

} // namespace

int main(int argc, char** argv) {
    write_goldens = argc > 1 && std::string_view(argv[1]) == "--write";
    std::ifstream in(TESSERA_SAFE_AREA_TSX, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(!text.empty(), "SafeArea.tsx is readable");
    auto fixture = parse_file("ui/SafeArea.tsx", text);
    auto result = lowering::lower_module(fixture->module_source(), "safe_area");
    check(result.has_value(), "SafeArea lowers: " + first_message(result));
    if (!result) {
        return jsx_test::finish("library safe area lowering");
    }
    const ir::Component* safe = find_component(*result, "SafeArea");
    check(safe != nullptr, "the SafeArea component exists");
    if (safe == nullptr) {
        return jsx_test::finish("library safe area lowering");
    }

    std::string props;
    for (const ir::Prop& prop : safe->props) {
        props += prop.name + ":" + prop.type + (prop.default_value ? "=" : "") + " ";
    }
    check(props == "edges:string= sources:string= minimum:int= mode:string= children:children ",
          "the declared props: " + props);

    const ir::Node& root = safe->root;
    check(root.props.count("role") == 0, "the wrapper has no role");
    check(root.props.count("className") == 1, "the props travel in the class tokens");
    check(root.children.size() == 1 && root.children[0].kind == ir::NodeKind::Slot,
          "the content is the Children slot");

    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    check(ids::assign_identity(module, "tessera-ui-test").has_value(), "identity assigns");
    const auto diagnostics = ir::validate(module);
    check(diagnostics.empty(), "the module validates: " +
                                   (diagnostics.empty() ? std::string() : diagnostics[0].message));
    check(codegen::generate(module).has_value(), "SafeArea generates C++");
    test_demo_project(text);
    return jsx_test::finish("library safe area lowering");
}
