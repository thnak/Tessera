// SPDX-License-Identifier: MIT
//
// The `Dispatch` action in generated C++ (ADR-066 item 4). The lowering half is in
// `lowering_reducer_tests`; this file checks the emitter half: a `dispatch({ type: "x" })` becomes
// a call of the handler its case became, resolved at generation time through the state's dispatch
// table.
//
// The IR is built directly rather than through the parser: the point here is the emitter, and a
// hand-built IR states the shape under test without a source file in the way.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/module.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
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

// A component with a reducer state, one handler per `cases` entry, and a third handler that
// dispatches `action`.
ir::IrModule reducer_module(std::vector<ir::DispatchCase> cases, std::string action) {
    ir::IrModule module;
    module.name = "demo";
    ir::Component component;
    component.id = "comp::Counter";
    component.component_id = "1";
    component.name = "Counter";
    component.source = span();
    // The state record, so a case's value has a C++ type. No `types` entry is needed for a
    // literal-only record because the emitter spells the struct name from the field.
    ir::RecordType type;
    type.id = "type::Counter";
    type.name = "Counter";
    type.source = span();
    type.members.push_back(ir::RecordMember{"count", "int", false});
    module.types.push_back(std::move(type));
    ir::State cell;
    cell.id = "state::state";
    cell.name = "state";
    cell.type = "Counter";
    {
        // The field list is built explicitly: a braced list of RecordField would have to copy a
        // unique_ptr value.
        std::vector<ir::RecordField> fields;
        fields.push_back(ir::RecordField{"count", ir::make_literal(std::int64_t{0})});
        cell.initial = ir::make_record("Counter", std::move(fields));
    }
    cell.dispatch = std::move(cases);
    component.state.push_back(std::move(cell));
    // One handler per case, each assigning the state.
    for (const ir::DispatchCase& entry : component.state.front().dispatch) {
        ir::Handler handler;
        handler.id = entry.handler;
        handler.source = span();
        ir::Action set;
        set.kind = ir::ActionKind::Set;
        set.target = "state::state";
        std::vector<ir::RecordField> fields;
        fields.push_back(ir::RecordField{"count", ir::make_literal(std::int64_t{0})});
        set.value = ir::make_record("Counter", std::move(fields));
        handler.actions.push_back(std::move(set));
        component.handlers.push_back(std::move(handler));
    }
    // The arrow that dispatches.
    ir::Handler arrow;
    arrow.id = "handler::9";
    arrow.source = span();
    ir::Action dispatch;
    dispatch.kind = ir::ActionKind::Dispatch;
    dispatch.target = "state::state";
    dispatch.action = std::move(action);
    arrow.actions.push_back(std::move(dispatch));
    component.handlers.push_back(std::move(arrow));
    ir::Node root;
    root.kind = ir::NodeKind::View;
    root.id = "node::1";
    root.source = span();
    root.events["onClick"] = "handler::9";
    component.root = std::move(root);
    module.components.push_back(std::move(component));
    return module;
}

// The generated text of the module, or an empty string with the error printed.
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

void test_dispatch_calls_the_cases_handler() {
    const std::string text = generate(reducer_module(
        {ir::DispatchCase{"inc", "handler::1"}, ir::DispatchCase{"reset", "handler::2"}}, "inc"));
    check(!text.empty(), "the module generates");
    // The arrow's body is a call of the `inc` case's handler, not a run-time table lookup.
    check(contains(text, "handler_1(tessera::ui::Event{});"),
          "the dispatch calls the handler its case became");
    check(!contains(text, "handler_2(tessera::ui::Event{});\n    }\n  }\n}\n\n} // namespace"),
          "it calls that case's handler and not another");
}

void test_dispatch_to_the_other_case() {
    const std::string text = generate(reducer_module(
        {ir::DispatchCase{"inc", "handler::1"}, ir::DispatchCase{"reset", "handler::2"}}, "reset"));
    check(contains(text, "handler_2(tessera::ui::Event{});"),
          "a different literal selects a different handler");
}

void test_an_unknown_literal_is_an_error() {
    // The dispatch table is the authority. A literal it does not list is a generation error rather
    // than a silently empty handler body, so this calls generate directly: the shared helper
    // prints the error it expects.
    const codegen::CodegenResult result =
        codegen::generate(reducer_module({ir::DispatchCase{"inc", "handler::1"}}, "nope"));
    check(!result.has_value(), "an unknown action literal is a generation error");
    if (!result) {
        check(result.error().front().find("is not a case of") != std::string::npos,
              "the error names the literal as not a case");
    }
}

void test_a_dotted_state_read_emits_the_field() {
    // A case that reads the cell it writes. The IR has no member-access expression, so the dot is
    // part of the id (ir-v1-expr.md section 4.3): the emitted C++ is the aggregate's own member.
    ir::IrModule module = reducer_module({ir::DispatchCase{"inc", "handler::1"}}, "inc");
    ir::Component& component = module.components.front();
    ir::Action set;
    set.kind = ir::ActionKind::Set;
    set.target = "state::state";
    std::vector<ir::RecordField> fields;
    fields.push_back(
        ir::RecordField{"count", ir::make_binary("+", ir::make_ref("state::state.count"),
                                                 ir::make_literal(std::int64_t{1}))});
    set.value = ir::make_record("Counter", std::move(fields));
    component.handlers.front().actions.clear();
    component.handlers.front().actions.push_back(std::move(set));
    const std::string text = generate(module);
    check(contains(text, "state_state_.count + 1"), "a dotted state read emits the member");
}

} // namespace

int main() {
    test_dispatch_calls_the_cases_handler();
    test_dispatch_to_the_other_case();
    test_an_unknown_literal_is_an_error();
    test_a_dotted_state_read_emits_the_field();
    if (failures == 0) {
        std::cout << "codegen reducer tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
