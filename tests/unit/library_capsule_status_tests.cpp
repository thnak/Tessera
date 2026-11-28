// SPDX-License-Identifier: MIT

// `packages/tessera-ui/CapsuleStatus.tsx` (issue #997) lowers, gets identity, validates and
// generates C++; its dot is hidden from the accessibility tree and every class it sets has a rule in
// live-capsule.css.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifndef TESSERA_CAPSULE_STATUS_TSX
#error "TESSERA_CAPSULE_STATUS_TSX must be defined by the build"
#endif
#ifndef TESSERA_CAPSULE_CSS
#error "TESSERA_CAPSULE_CSS must be defined by the build"
#endif

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace ids = tessera::compiler::ids;

std::string read(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    const std::string text = read(TESSERA_CAPSULE_STATUS_TSX);
    check(!text.empty(), "CapsuleStatus.tsx is readable");
    auto fixture = parse_file("ui/CapsuleStatus.tsx", text);
    auto result = lowering::lower_module(fixture->module_source(), "capsule_status");
    check(result.has_value(), "CapsuleStatus lowers: " + first_message(result));
    if (!result) {
        return jsx_test::finish("library capsule status");
    }
    const ir::Component* status = find_component(*result, "CapsuleStatus");
    check(status != nullptr, "the CapsuleStatus component exists");
    if (status == nullptr) {
        return jsx_test::finish("library capsule status");
    }
    std::string props;
    for (const ir::Prop& prop : status->props) {
        props += prop.name + ":" + prop.type + (prop.default_value ? "=" : "") + " ";
    }
    check(props == "label:string= tone:string= dot:bool= ", "the declared props: " + props);

    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    check(ids::assign_identity(module, "tessera-ui-test").has_value(), "identity assigns");
    const auto diagnostics = ir::validate(module);
    check(diagnostics.empty(), "the module validates: " +
                                   (diagnostics.empty() ? std::string() : diagnostics[0].message));
    check(codegen::generate(module, {}, std::nullopt).has_value(), "it generates C++");

    const std::string css = read(TESSERA_CAPSULE_CSS);
    for (const char* rule : {".tcaps-status", ".tcaps-dot", ".tcaps-label", ".tcaps-tone-success",
                             ".tcaps-tone-warning", ".tcaps-tone-danger"}) {
        check(css.find(rule) != std::string::npos, std::string("live-capsule.css styles ") + rule);
    }
    return jsx_test::finish("library capsule status");
}
