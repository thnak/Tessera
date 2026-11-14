// SPDX-License-Identifier: MIT

// A component with a `{children}` slot in a project with an id salt (issue #239): identity
// assignment gives the Slot a `uiId` but no automation id, so the module validates. A Slot is a
// bare position marker and the validator rejects a Slot that carries an automation id.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <string>
#include <utility>

namespace {

using namespace lowering_module_test;
namespace ids = tessera::compiler::ids;

constexpr const char* kCard = "import { Children } from \"tessera\";\n"
                              "export type CardProps = { children: Children };\n"
                              "export function Card({ children }: CardProps) {\n"
                              "  return <View>{children}</View>;\n"
                              "}\n";

void test_slot_in_a_salted_module() {
    auto fixture = parse_file("ui/Card.tsx", kCard);
    auto result = lowering::lower_module(fixture->module_source(), "slot");
    check(result.has_value(), "the Card lowers: " + first_message(result));
    if (!result) {
        return;
    }
    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    const auto assigned = ids::assign_identity(module, "slot-salt");
    check(assigned.has_value(), "identity assigns");
    const auto diagnostics = ir::validate(module);
    check(diagnostics.empty(), "a salted module with a Slot validates: " +
                                   (diagnostics.empty() ? std::string() : diagnostics[0].message));
    const ir::Node& slot = module.components[0].root.children[0];
    check(slot.kind == ir::NodeKind::Slot, "the child is the Slot");
    check(!slot.ui_id.empty() && slot.automation_id.empty(),
          "the Slot has a uiId and no automation id");
    check(!module.components[0].root.automation_id.empty(),
          "an ordinary node still mirrors its uiId");
}

} // namespace

int main() {
    test_slot_in_a_salted_module();
    return jsx_test::finish("ir ids slot");
}
