// SPDX-License-Identifier: MIT

// `packages/tessera-ui/Button.tsx` (issue #239) lowers, gets identity under a salt, validates and
// generates C++, and its IR carries the accessibility contract: a focusable `button` named by the
// `label` prop that reports `aria-disabled`, and a click that is guarded by `disabled` before it
// calls `onPress`.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifndef TESSERA_BUTTON_TSX
#error "TESSERA_BUTTON_TSX must be defined by the build"
#endif

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace ids = tessera::compiler::ids;

std::string prop_kind(const ir::Node& node, const char* name) {
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

} // namespace

int main() {
    std::ifstream in(TESSERA_BUTTON_TSX, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(!text.empty(), "Button.tsx is readable");
    auto fixture = parse_file("ui/Button.tsx", text);
    auto result = lowering::lower_module(fixture->module_source(), "button");
    check(result.has_value(), "Button lowers: " + first_message(result));
    if (!result) {
        return jsx_test::finish("library button lowering");
    }
    const ir::Component* button = find_component(*result, "Button");
    check(button != nullptr, "the Button component exists");
    if (button == nullptr) {
        return jsx_test::finish("library button lowering");
    }

    // Props: label and onPress are required, variant/size/disabled have defaults, children is the
    // slot.
    std::string props;
    for (const ir::Prop& prop : button->props) {
        props += prop.name + ":" + prop.type + (prop.default_value ? "=" : "") + " ";
    }
    check(props == "label:string onPress:callback variant:string= size:string= disabled:bool= "
                   "children:children ",
          "the declared props: " + props);

    const ir::Node& root = button->root;
    check(prop_kind(root, "role") == "button", "the Button is a button");
    check(prop_kind(root, "aria-label") == "<expr>", "the name is the label prop");
    check(prop_kind(root, "aria-disabled") == "<expr>", "aria-disabled follows the disabled prop");
    check(root.props.count("tabIndex") == 1, "the Button is focusable");
    check(root.children.size() == 1 && root.children[0].kind == ir::NodeKind::Slot,
          "the content is the Children slot");

    // Handlers in source order: down, up, leave, key down, key up, click.
    check(button->handlers.size() == 6, "six handlers");
    if (button->handlers.size() == 6) {
        const auto& down = button->handlers[0].actions;
        check(down.size() == 1 && down[0].guard != nullptr,
              "pointer down is guarded by `!disabled`");
        const auto& click = button->handlers[5].actions;
        check(click.size() == 1 && click[0].kind == ir::ActionKind::CallProp &&
                  click[0].target == "prop::onPress" && click[0].guard != nullptr,
              "the click calls onPress behind the `!disabled` guard");
    }

    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    check(ids::assign_identity(module, "tessera-ui-test").has_value(), "identity assigns");
    const auto diagnostics = ir::validate(module);
    check(diagnostics.empty(), "the module validates: " +
                                   (diagnostics.empty() ? std::string() : diagnostics[0].message));
    const auto generated = codegen::generate(module);
    check(generated.has_value(),
          "the Button generates C++: " +
              (generated || generated.error().empty() ? std::string() : generated.error().front()));
    return jsx_test::finish("library button lowering");
}
