// SPDX-License-Identifier: MIT
//
// Behavior hook lowering (ADR-064 item 7, spike #658; docs/specs/behavior-hooks-v1.md section 6).
//
// The analyzer checks the call against the built-in hook manifest; this proves the IR the lowering
// produces: the id, the targets, the outputs, and that a component without a behavior call writes
// no `behaviors` field at all (which is what keeps every committed IR golden unchanged).

#include "lowering_hooks_test_util.hpp"

#include <iostream>
#include <string>

namespace {

using lowering_hooks_test::check;
using lowering_hooks_test::component;
using lowering_hooks_test::failures;
using lowering_hooks_test::lower;
using lowering_hooks_test::parse;
using lowering_hooks_test::parse_raw;
namespace ir = tessera::compiler::ir;

// A component that calls `usePressFeedback` on a ref lowers to one `behaviors` entry with the
// registry name, the target ref and the three outputs.
void test_press_feedback_lowers() {
    {
        const auto& manifest = tessera::compiler::rules::builtin_hook_manifest();
        check(manifest.find("usePressFeedback") != nullptr,
              "the built-in manifest declares usePressFeedback (hooks: " +
                  std::to_string(manifest.hooks.size()) + ")");
    }
    const auto probe = lowering_hooks_test::parse_raw(component(
        "  const button = useRef<View>(null);\n  const press = usePressFeedback(button, {});\n"));
    if (!probe->parsed.ok()) {
        check(false, "parse: " + std::string(probe->parsed.diagnostics.front().message));
        return;
    }
    const auto fixture = parse(component(
        "  const button = useRef<View>(null);\n  const press = usePressFeedback(button, {});\n"));
    const auto lowered = lower(*fixture);
    if (!lowered) {
        check(false, "the fixture lowers: " + lowering_hooks_test::first_code(lowered));
        return;
    }
    check(lowered->behaviors.size() == 1, "one behavior is lowered");
    if (lowered->behaviors.empty()) {
        return;
    }
    const ir::Behavior& behavior = lowered->behaviors.front();
    check(behavior.id == "beh::press", "the id is `beh::<name>`");
    check(behavior.hook == "usePressFeedback", "the hook name is carried");
    check(behavior.behavior == "press_feedback", "the registry name is carried");
    check(behavior.targets.size() == 1 && behavior.targets.front() == "ref::button",
          "the target is the `ref::` id");
    check(behavior.outputs.size() == 3, "the three outputs are carried");
}

// A component that calls `useFocusTrap` lowers to one exclusive `behaviors` entry with the two
// options written out (including the defaulted one), so a consumer needs no manifest to evaluate
// it.
void test_focus_trap_lowers() {
    const auto fixture = parse(component(
        "  const box = useRef<View>(null);\n  const [open, setOpen] = useState<boolean>(false);\n"
        "  const trap = useFocusTrap(box, { active: open });\n"));
    const auto lowered = lower(*fixture);
    if (!lowered) {
        check(false, "the focus-trap fixture lowers: " + lowering_hooks_test::first_code(lowered));
        return;
    }
    check(lowered->behaviors.size() == 1, "one behavior is lowered");
    if (lowered->behaviors.empty()) {
        return;
    }
    const ir::Behavior& behavior = lowered->behaviors.front();
    check(behavior.id == "beh::trap", "the id is `beh::<name>`");
    check(behavior.behavior == "focus_trap", "the registry name is carried");
    check(behavior.exclusive, "the manifest's exclusivity is copied into the IR");
    check(behavior.targets.size() == 1 && behavior.targets.front() == "ref::box",
          "the target is the `ref::` id");
    check(behavior.outputs.size() == 1 && behavior.outputs.front().name == "active",
          "the `active` output is carried");
    check(behavior.args.size() == 2, "both options are written out, the defaulted one included");
    if (behavior.args.size() == 2) {
        check(behavior.args[0].name == "active" && behavior.args[0].expr.kind == ir::ExprKind::Ref,
              "the reactive option keeps its expression");
        check(behavior.args[1].name == "restoreFocus" &&
                  behavior.args[1].expr.kind == ir::ExprKind::Literal,
              "the defaulted option is written as a literal");
    }
}

// A component without a behavior call writes no `behaviors` field, so every committed IR golden of
// such a component is byte-identical.
void test_no_behavior_writes_no_field() {
    const auto fixture = parse(component("  const [count, setCount] = useState<int>(0);\n"));
    const auto lowered = lower(*fixture);
    if (!lowered) {
        check(false, "the fixture lowers: " + lowering_hooks_test::first_code(lowered));
        return;
    }
    check(lowered->behaviors.empty(), "a component without a behavior lowers none");
}

// A bare statement (no `const` binding) is numbered from 1, which is the rule the spec states for
// `beh::<hook>_<n>`.
void test_bare_statement_is_numbered() {
    const auto fixture =
        parse(component("  const button = useRef<View>(null);\n  usePressFeedback(button, {});\n"));
    const auto lowered = lower(*fixture);
    if (!lowered || lowered->behaviors.empty()) {
        check(false, "the fixture lowers one behavior");
        return;
    }
    check(lowered->behaviors.front().id == "beh::usePressFeedback_1",
          "a bare statement is numbered from 1");
}

// A hook the manifest does not declare is still an unknown hook, so the manifest is the authority
// rather than a second list in the analyzer.
void test_unknown_hook_is_rejected() {
    const auto fixture = parse_raw(component(
        "  const button = useRef<View>(null);\n  const trap = useNothing(button, {});\n"));
    check(fixture->analyzed.has_errors(), "an undeclared hook is rejected by the analysis");
}

// The options object is required to be an object literal, so a spread or a computed key cannot
// reach the lowering.
void test_options_must_be_an_object() {
    const auto fixture = parse_raw(component("  const button = useRef<View>(null);\n  const press "
                                             "= usePressFeedback(button, disabled);\n"));
    check(fixture->analyzed.has_errors(), "a non-object options argument is rejected");
}

} // namespace

int main() {
    test_press_feedback_lowers();
    test_focus_trap_lowers();
    test_no_behavior_writes_no_field();
    test_bare_statement_is_numbered();
    test_unknown_hook_is_rejected();
    test_options_must_be_an_object();
    if (failures != 0) {
        std::cerr << "lowering_behaviors: FAILED\n";
        return 1;
    }
    std::cout << "lowering_behaviors: ok\n";
    return 0;
}
