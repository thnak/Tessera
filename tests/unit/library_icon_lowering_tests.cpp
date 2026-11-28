// SPDX-License-Identifier: MIT

// `packages/tessera-ui/Icon.tsx` (issue #530) lowers with the generated glyph table
// (`icons/IconGlyph.g.tsx`), gets identity under a salt, validates and generates C++. Its IR
// carries the accessibility contract (a named icon is an `img` with its label; an unnamed one is
// hidden), the glyph table holds a `currentColor` `Path` per imported path icon, and a demo that
// puts an Icon inside a Button generates the committed goldens the runtime test compiles.

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
#ifndef TESSERA_LIBRARY_DIR
#error "TESSERA_LIBRARY_DIR must be defined by the build"
#endif

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace ids = tessera::compiler::ids;

bool write_goldens = false;

std::string read(const std::string& relative) {
    std::ifstream in(std::filesystem::path(TESSERA_LIBRARY_DIR) / relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string prop_text(const ir::Node& node, const char* name) {
    const auto found = node.props.find(name);
    if (found == node.props.end() || !found->second) {
        return "<absent>";
    }
    if (found->second->kind == ir::ExprKind::Literal) {
        if (const auto* text = std::get_if<std::string>(&found->second->literal)) {
            return *text;
        }
        return "<literal>";
    }
    return "<expr>";
}

// An Icon in the three roles the runtime test drives: named, directional and small, and decorative
// inside a Button.
constexpr const char* kDemo =
    "import { useState } from \"tessera\";\n"
    "import { Button } from \"./Button\";\n"
    "import { Icon } from \"./Icon\";\n"
    "export function IconDemo() {\n"
    "  const [count, setCount] = useState<int>(0);\n"
    "  return (\n"
    "    <View>\n"
    "      <Icon name=\"tabler:home\" label=\"Home\" />\n"
    "      <Icon name=\"tabler:chevron-right\" size=\"sm\" mirror={true} />\n"
    "      <Button label=\"Save\" onPress={() => setCount(count + 1)}>\n"
    "        <Icon name=\"tabler:check\" />\n"
    "      </Button>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

void check_goldens(const std::vector<codegen::GeneratedFile>& files) {
    for (const codegen::GeneratedFile& file : files) {
        // Its own directory: the Button here is generated into the `icon_demo` namespace, not the
        // `button_demo` one of library_button_lowering_tests.
        const std::filesystem::path path =
            std::filesystem::path(TESSERA_CODEGEN_GOLDEN_DIR) / "icon" / file.path;
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

std::size_t count_of(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    write_goldens = argc > 1 && std::string_view(argv[1]) == "--write";
    const std::string icon_source = read("Icon.tsx");
    const std::string glyph_source = read("icons/IconGlyph.g.tsx");
    const std::string button_source = read("Button.tsx");
    check(!icon_source.empty() && !glyph_source.empty(),
          "Icon.tsx and the glyph table are readable");

    // The glyph table: one currentColor Path per path of every representable icon, none for the
    // icon the tool listed as not representable (tabler:search has a circle).
    check(count_of(glyph_source, "<Path") == 9, "the glyph table holds nine Path nodes");
    check(count_of(glyph_source, "fill=\"currentColor\"") +
                  count_of(glyph_source, "stroke=\"currentColor\"") ==
              9,
          "every glyph path paints with currentColor");
    check(glyph_source.find("name === \"tabler:search\"") == std::string::npos,
          "an icon the tool could not represent is absent, not redrawn");

    auto glyph_fixture = parse_file("ui/icons/IconGlyph.g.tsx", glyph_source);
    auto glyph = lowering::lower_module(glyph_fixture->module_source(), "icon_glyph");
    check(glyph.has_value(), "the glyph table lowers: " + first_message(glyph));

    Project project;
    project.add(parse_file("ui/icons/IconGlyph.g.tsx", glyph_source));
    project.add(parse_file("ui/Icon.tsx", icon_source));
    project.add(parse_file("ui/Button.tsx", button_source));
    project.add(parse_file("ui/IconDemo.tsx", kDemo));
    auto result = project.lower("icon_demo");
    check(result.has_value(), "the demo project lowers: " + first_message(result));
    if (!result) {
        return jsx_test::finish("library icon lowering");
    }

    const ir::Component* icon = find_component(*result, "Icon");
    check(icon != nullptr, "the Icon component exists");
    if (icon != nullptr) {
        std::string props;
        for (const ir::Prop& prop : icon->props) {
            props += prop.name + ":" + prop.type + (prop.default_value ? "=" : "") + " ";
        }
        check(props == "name:string label:string= size:string= mirror:bool= ",
              "the declared props: " + props);
        // Two guarded boxes: the named one is an `img` carrying the label, the unnamed one is a
        // `presentation` box that is `aria-hidden`. Each draws one glyph.
        const auto& boxes = icon->root.children;
        check(boxes.size() == 2, "the Icon has a named and a decorative box");
        if (boxes.size() == 2) {
            check(prop_text(boxes[0], "role") == "img" &&
                      prop_text(boxes[0], "aria-label") == "<expr>",
                  "a named icon is an img named by the label prop");
            check(boxes[0].children.size() == 1 && boxes[1].children.size() == 1,
                  "each box draws one glyph");
            check(prop_text(boxes[1], "role") == "presentation" &&
                      boxes[1].props.count("aria-hidden") == 1 &&
                      boxes[1].props.count("aria-label") == 0,
                  "an unnamed icon is hidden from the accessibility tree");
            check(boxes[0].condition != nullptr && boxes[1].condition != nullptr,
                  "exactly one box exists for a given label");
        }
    }

    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    check(ids::assign_identity(module, "icon-demo").has_value(), "identity assigns");
    const auto diagnostics = ir::validate(module);
    check(diagnostics.empty(), "the demo module validates: " +
                                   (diagnostics.empty() ? std::string() : diagnostics[0].message));
    const auto generated = codegen::generate(module, {}, std::nullopt);
    check(generated.has_value(),
          "the Icon demo generates C++: " +
              (generated || generated.error().empty() ? std::string() : generated.error().front()));
    if (generated) {
        check_goldens(*generated);
    }
    return jsx_test::finish("library icon lowering");
}
