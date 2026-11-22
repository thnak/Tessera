// SPDX-License-Identifier: MIT
//
// The `If` action in generated C++ (ADR-066 item 2). The lowering half is in
// `lowering_handler_control_tests`; this file checks that the emitted handler body is a real
// `if`/`else` whose branches are the emitted actions, because the ADR's differential property is
// that the evaluator and the generated code agree.
//
// The IR is built directly rather than through the parser: the point here is the emitter, and a
// hand-built IR states the shape under test without a source file in the way.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;

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

ir::SourceSpan span() {
    ir::SourceSpan value;
    value.file = "ui/Counter.tsx";
    value.offset = 0;
    value.length = 10;
    value.line = 1;
    value.column = 1;
    value.end_line = 1;
    value.end_column = 11;
    return value;
}

// A component with one boolean state and one handler whose actions are `actions`.
ir::IrModule module_with_handler(std::vector<ir::Action> actions) {
    ir::IrModule module;
    module.name = "demo";
    ir::Component component;
    component.id = "comp::Counter";
    component.component_id = "1";
    component.name = "Counter";
    component.source = span();
    component.props.push_back(ir::Prop{"disabled", "bool", nullptr});
    component.state.push_back(ir::State{"state::busy", "busy", "bool", ir::make_literal(false)});
    ir::Handler handler;
    handler.id = "handler::1";
    handler.actions = std::move(actions);
    handler.source = span();
    component.handlers.push_back(std::move(handler));
    ir::Node root;
    root.kind = ir::NodeKind::View;
    root.id = "node::1";
    root.source = span();
    root.events["onClick"] = "handler::1";
    component.root = std::move(root);
    module.components.push_back(std::move(component));
    return module;
}

// The generated base of the module, or an empty string with the error printed.
std::string generate(const ir::IrModule& module) {
    const codegen::CodegenResult result = codegen::generate(module);
    if (!result) {
        std::cerr << "FAIL: codegen: " << result.error().front() << "\n";
        ++failures;
        return {};
    }
    std::string out;
    for (const codegen::GeneratedFile& file : *result) {
        out += file.text;
    }
    return out;
}

// `set state::busy = <value>`.
ir::Action set_busy(ir::LiteralValue value) {
    ir::Action action;
    action.kind = ir::ActionKind::Set;
    action.target = "state::busy";
    action.value = ir::make_literal(std::move(value));
    return action;
}

void test_if_else_emits_a_cpp_if() {
    ir::Action branch;
    branch.kind = ir::ActionKind::If;
    branch.guard = ir::make_ref("prop::disabled");
    branch.then_actions.push_back(set_busy(false));
    branch.else_actions.push_back(set_busy(true));
    std::vector<ir::Action> actions;
    actions.push_back(std::move(branch));
    const std::string text = generate(module_with_handler(std::move(actions)));
    check(contains(text, "if (prop_disabled_) {"), "the condition is the emitted guard");
    check(contains(text, "} else {"), "an else branch is emitted");
    // Both branches assign the same state, so the two assignments are what proves both were
    // emitted rather than one being dropped.
    std::size_t assignments = 0;
    for (std::size_t at = text.find("state_busy_ = "); at != std::string::npos;
         at = text.find("state_busy_ = ", at + 1)) {
        ++assignments;
    }
    check(assignments >= 2, "both branches assign the state");
}

void test_bare_if_has_no_else() {
    ir::Action branch;
    branch.kind = ir::ActionKind::If;
    branch.guard = ir::make_ref("prop::disabled");
    branch.then_actions.push_back(set_busy(true));
    std::vector<ir::Action> actions;
    actions.push_back(std::move(branch));
    const std::string text = generate(module_with_handler(std::move(actions)));
    check(contains(text, "if (prop_disabled_) {"), "the condition is emitted");
    check(!contains(text, "} else {"), "a bare `if` emits no else branch");
}

void test_nested_if() {
    ir::Action inner;
    inner.kind = ir::ActionKind::If;
    inner.guard = ir::make_ref("state::busy");
    inner.then_actions.push_back(set_busy(false));
    ir::Action outer;
    outer.kind = ir::ActionKind::If;
    outer.guard = ir::make_ref("prop::disabled");
    outer.then_actions.push_back(std::move(inner));
    std::vector<ir::Action> actions;
    actions.push_back(std::move(outer));
    const std::string text = generate(module_with_handler(std::move(actions)));
    check(contains(text, "if (prop_disabled_) {"), "the outer condition is emitted");
    check(contains(text, "if (state_busy_) {"), "the inner condition is emitted");
}

void test_if_without_a_condition_is_rejected() {
    // An `If` with no guard has no condition to test, so codegen refuses it rather than emitting an
    // `if ()` that would not compile. This is checked through `generate` directly, because the
    // helper treats an error as a test failure.
    ir::Action branch;
    branch.kind = ir::ActionKind::If;
    branch.then_actions.push_back(set_busy(true));
    std::vector<ir::Action> actions;
    actions.push_back(std::move(branch));
    const codegen::CodegenResult result =
        codegen::generate(module_with_handler(std::move(actions)));
    check(!result.has_value(), "an `if` with no condition produces no code");
}

void test_validation_rejects_a_malformed_if() {
    // The validator is what a hand-written or tool-generated IR meets first, so it must reject an
    // `If` that carries a target or a value: those belong to the other kinds.
    ir::Action branch;
    branch.kind = ir::ActionKind::If;
    branch.guard = ir::make_ref("prop::disabled");
    branch.target = "state::busy";
    std::vector<ir::Action> actions;
    actions.push_back(std::move(branch));
    const ir::IrModule module = module_with_handler(std::move(actions));
    const std::vector<ir::IrDiagnostic> diagnostics = ir::validate(module);
    check(!diagnostics.empty(), "an `if` with a target is rejected");
}

} // namespace

int main() {
    test_if_else_emits_a_cpp_if();
    test_bare_if_has_no_else();
    test_nested_if();
    test_if_without_a_condition_is_rejected();
    test_validation_rejects_a_malformed_if();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen handler control tests passed\n";
    return 0;
}
