// SPDX-License-Identifier: MIT

// `Path` `currentColor` is a tint (issue #530): the synthesized document paints white, the emitted
// payload says to tint with the node's `color`, a literal-color `Path` does not tint, and a
// `currentColor` paint mixed with a literal color on one node is refused (a tint colors a whole
// document).

#include "codegen_vector_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
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

ir::Node& explicit_node(ir::IrModule& module) {
    return module.components[0].root.children[0];
}

std::string first_error(
    const std::expected<std::vector<codegen::GeneratedFile>, std::vector<std::string>>& result) {
    return result.has_value() || result.error().empty() ? std::string() : result.error().front();
}

} // namespace

int main() {
    ir::IrModule module = make_vector_module();
    explicit_node(module).props["fill"] = ir::make_literal(std::string("currentColor"));
    explicit_node(module).props["stroke"] = ir::make_literal(std::string("none"));
    const auto result = codegen::generate(module, {}, std::nullopt);
    check(result.has_value(), "a currentColor Path generates: " + first_error(result));
    if (result) {
        const codegen::GeneratedFile* source = find_file(*result, "VectorIcon.g.hpp");
        check(source != nullptr &&
                  contains(source->text, "paint_node_2.vector.tint_current_color = true;"),
              "the payload tints with the node color");
        check(source != nullptr &&
                  !contains(source->text, "paint_node_3.vector.tint_current_color"),
              "a literal-color Path does not tint");
    }

    ir::IrModule mixed = make_vector_module();
    explicit_node(mixed).props["fill"] = ir::make_literal(std::string("CURRENTCOLOR"));
    explicit_node(mixed).props["stroke"] = ir::make_literal(std::string("#102030"));
    const auto refused = codegen::generate(mixed, {}, std::nullopt);
    check(!refused.has_value() && contains(first_error(refused), "mixes `currentColor`"),
          "currentColor mixed with a literal color is refused");

    if (failures != 0) {
        return 1;
    }
    std::cout << "vector tint codegen tests passed\n";
    return 0;
}
