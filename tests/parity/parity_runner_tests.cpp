// SPDX-License-Identifier: MIT
//
// The preview-parity runner itself (issue #7): the script format, the IR side, and the mismatch
// report, driven by a fake native side so each failure shape is checked in isolation.

#include "parity_runner.hpp"
#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ir = tessera::compiler::ir;
namespace parity = tessera::parity;
namespace ui = tessera::ui;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "parity_runner: " << message << "\n";
    }
    return condition;
}

// state::count (int, 0), state::label (string, "idle"); handler::1 adds `by` to count and
// bind::tag (a native binding with a preview body) sets the label.
ir::IrModule make_module() {
    ir::IrModule module;
    ir::Component component;
    component.id = "component::Counter";
    component.name = "Counter";
    component.state.push_back({"state::count", "count", "int", ir::make_literal(0)});
    component.state.push_back({"state::label", "label", "string", ir::make_literal("idle")});

    ir::Handler add;
    add.id = "handler::1";
    add.params.push_back({"by", "int"});
    ir::Action action;
    action.target = "state::count";
    action.value = ir::make_binary("+", ir::make_ref("state::count"), ir::make_ref("by"));
    add.actions.push_back(std::move(action));
    component.handlers.push_back(std::move(add));

    // Node 201 clicks into bind::tag, 202 into handler::1 (which needs an argument), 203 has no
    // onClick.
    const auto node = [](const char* automation_id, const char* handler) {
        ir::Node child;
        child.automation_id = automation_id;
        if (handler != nullptr) {
            child.events["onClick"] = handler;
        }
        return child;
    };
    component.root.children.push_back(node("201", "bind::tag"));
    component.root.children.push_back(node("202", "handler::1"));
    component.root.children.push_back(node("203", nullptr));
    module.components.push_back(std::move(component));

    ir::Binding tag;
    tag.id = "bind::tag";
    tag.component = "Counter";
    tag.kind = ir::BindingKind::Event;
    ir::Preview preview;
    ir::Action set_label;
    set_label.target = "state::label";
    set_label.value = ir::make_literal("tagged");
    preview.actions.push_back(std::move(set_label));
    tag.preview = std::move(preview);
    module.bindings.push_back(std::move(tag));

    ir::Binding bare;
    bare.id = "bind::nothing";
    bare.component = "Counter";
    module.bindings.push_back(std::move(bare));
    return module;
}

// A native side that keeps its own state and applies `behavior` per step.
struct FakeNative {
    int count = 0;
    std::string label = "idle";
    int tag_adds = 0; // how much `bind::tag` also adds to the count (a deliberate bug when not 0)
    int scale = 1;    // multiplies the argument of handler::1

    std::expected<ui::StateSnapshot, std::string> operator()(const parity::Step* step) {
        if (step != nullptr) {
            if (step->click && step->target == "201") {
                label = "tagged";
                count += tag_adds;
            } else if (step->click) {
                return std::unexpected("native side has no node " + step->target);
            } else if (step->target == "handler::1") {
                count += scale * *step->args.at(0).as_int();
            } else if (step->target == "bind::tag") {
                label = "tagged";
                count += tag_adds;
            } else if (step->target != "bind::nothing") {
                return std::unexpected("native side has no handler " + step->target);
            }
        }
        ui::StateSnapshot snapshot;
        snapshot.push_back(ui::state_cell("state::count", count));
        snapshot.push_back(ui::state_cell("state::label", label));
        return snapshot;
    }
};

std::vector<parity::Step> script_of(const char* text) {
    auto steps = parity::parse_script(text);
    if (!steps) {
        std::cerr << "parity_runner: bad test script: " << steps.error() << "\n";
        return {};
    }
    return std::move(*steps);
}

} // namespace

int main() {
    bool ok = true;

    // --- script format ------------------------------------------------------------------------
    {
        auto steps =
            parity::parse_script("# comment\n\ncall handler::1 3\r\ncall bind::tag\n"
                                 "call handler::2 -4 1.5 true \"a \\\"b\\\" \\\\c\" 2e2\n");
        ok = expect(steps.has_value() && steps->size() == 3, "three steps, comments skipped") && ok;
        if (steps && steps->size() == 3) {
            ok = expect((*steps)[0].target == "handler::1" && (*steps)[0].args.size() == 1 &&
                            *(*steps)[0].args[0].as_int() == 3,
                        "an int argument") &&
                 ok;
            const auto& rich = (*steps)[2].args;
            ok = expect(rich.size() == 5 && *rich[0].as_int() == -4 && *rich[1].as_float() == 1.5 &&
                            *rich[2].as_bool() && *rich[3].as_string() == "a \"b\" \\c" &&
                            *rich[4].as_float() == 200.0,
                        "int, float, bool, escaped string and exponent arguments") &&
                 ok;
        }
        ok = expect(!parity::parse_script("click handler::1").has_value(),
                    "a step must start with call") &&
             ok;
        ok = expect(!parity::parse_script("call").has_value(), "call needs an id") && ok;
        ok = expect(!parity::parse_script("call handler::1 \"open").has_value(),
                    "an unterminated string is an error") &&
             ok;
        ok = expect(!parity::parse_script("call handler::1 99999999999").has_value(),
                    "an int outside 32 bits is an error") &&
             ok;
        const auto bad = parity::parse_script("call a\ncall b nope");
        ok = expect(!bad.has_value() && bad.error().find("line 2") != std::string::npos,
                    "the error names the script line") &&
             ok;
    }

    const ir::IrModule module = make_module();
    const ir::Component& component = module.components.front();

    // --- initial state ------------------------------------------------------------------------
    {
        const auto scope = parity::initial_scope(component);
        ok = expect(scope.has_value() && *scope->at("state::count").as_int() == 0 &&
                        *scope->at("state::label").as_string() == "idle",
                    "initial scope holds the evaluated initials") &&
             ok;
    }

    // --- agreeing runs ------------------------------------------------------------------------
    {
        FakeNative native;
        const parity::Report report =
            parity::run_parity(module, component,
                               script_of("call handler::1 2\ncall bind::tag\ncall handler::1 5\n"
                                         "call bind::nothing\n"),
                               std::ref(native));
        ok = expect(report.ok() && report.steps_run == 4,
                    "an agreeing run is ok: " + parity::format_report(report)) &&
             ok;
    }

    // --- a mismatch names the step, the binding and the cell ---------------------------------
    {
        FakeNative native;
        native.tag_adds = 1;
        const parity::Report report = parity::run_parity(
            module, component, script_of("call handler::1 2\ncall bind::tag\ncall handler::1 5\n"),
            std::ref(native));
        ok = expect(!report.ok() && report.error.empty() && report.mismatches.size() == 1 &&
                        report.steps_run == 2,
                    "the run stops at the first difference") &&
             ok;
        if (report.mismatches.size() == 1) {
            const parity::Mismatch& m = report.mismatches.front();
            ok = expect(m.step == 1 && m.target == "bind::tag" && m.cell == "state::count" &&
                            m.preview == "2" && m.native == "3",
                        "the mismatch names step 1, bind::tag, state::count, 2 versus 3") &&
                 ok;
        }
        const std::string text = parity::format_report(report);
        ok = expect(text == "step 1 (bind::tag): state::count is 2 in the preview and 3 in the "
                            "native code\n",
                    "report text: " + text) &&
             ok;
    }

    // --- click steps resolve through the IR node's onClick ----------------------------------
    {
        const auto steps = parity::parse_script("click 201\ncall handler::1 4\nclick 201");
        ok = expect(steps.has_value() && steps->size() == 3 && (*steps)[0].click &&
                        (*steps)[0].target == "201" && !(*steps)[1].click,
                    "click and call steps parse") &&
             ok;
        ok = expect(!parity::parse_script("click bind::tag").has_value() &&
                        !parity::parse_script("click").has_value() &&
                        !parity::parse_script("click 201 3").has_value(),
                    "a click takes one numeric automation id") &&
             ok;

        FakeNative agree;
        parity::Report report = parity::run_parity(
            module, component, script_of("click 201\ncall handler::1 4"), std::ref(agree));
        ok = expect(report.ok() && report.steps_run == 2,
                    "a click runs the preview body of the node's handler: " +
                        parity::format_report(report)) &&
             ok;

        FakeNative buggy;
        buggy.tag_adds = 1;
        report = parity::run_parity(module, component, script_of("click 201"), std::ref(buggy));
        ok = expect(parity::format_report(report) ==
                        "step 0 (click 201 (bind::tag)): state::count is 0 in the preview and 1 "
                        "in the native code\n",
                    "a click mismatch names the automation id and the handler: " +
                        parity::format_report(report)) &&
             ok;

        FakeNative other;
        report = parity::run_parity(module, component, script_of("click 999"), std::ref(other));
        ok = expect(report.error.find("no node with that automation id") != std::string::npos,
                    "an unknown automation id is a run error") &&
             ok;
        report = parity::run_parity(module, component, script_of("click 203"), std::ref(other));
        ok = expect(report.error.find("no node with that automation id and an onClick") !=
                        std::string::npos,
                    "a node without onClick is a run error") &&
             ok;
        report = parity::run_parity(module, component, script_of("click 202"), std::ref(other));
        ok = expect(report.error.find("takes 1 argument") != std::string::npos,
                    "a click on a handler that needs arguments is a run error") &&
             ok;
    }

    // --- a wrong initial state is reported before any step -----------------------------------
    {
        FakeNative native;
        native.count = 7;
        const parity::Report report = parity::run_parity(
            module, component, script_of("call bind::nothing"), std::ref(native));
        ok = expect(report.mismatches.size() == 1 && report.mismatches[0].step == -1 &&
                        report.steps_run == 0,
                    "the initial state is compared first") &&
             ok;
    }

    // --- a differing int/float type is a mismatch --------------------------------------------
    {
        const auto typed =
            [](const parity::Step*) -> std::expected<ui::StateSnapshot, std::string> {
            ui::StateSnapshot snapshot;
            snapshot.push_back(ui::state_cell("state::count", 0.0)); // a double, the IR has an int
            snapshot.push_back(ui::state_cell("state::label", std::string("idle")));
            return snapshot;
        };
        const parity::Report report =
            parity::run_parity(module, component, script_of("call bind::nothing"), typed);
        ok = expect(report.mismatches.size() == 1 && report.mismatches[0].cell == "state::count",
                    "int 0 and float 0 are different values") &&
             ok;
    }

    // --- a cell the IR does not have ---------------------------------------------------------
    {
        const auto extra =
            [](const parity::Step*) -> std::expected<ui::StateSnapshot, std::string> {
            ui::StateSnapshot snapshot;
            snapshot.push_back(ui::state_cell("state::ghost", 1));
            return snapshot;
        };
        const parity::Report report = parity::run_parity(module, component, {}, extra);
        ok = expect(report.mismatches.size() == 1 && report.mismatches[0].cell == "state::ghost",
                    "a native cell with no IR counterpart is a mismatch") &&
             ok;
    }

    // --- run errors are not mismatches -------------------------------------------------------
    {
        FakeNative native;
        parity::Report report =
            parity::run_parity(module, component, script_of("call handler::9"), std::ref(native));
        ok = expect(!report.ok() && report.mismatches.empty() &&
                        report.error.find("no handler or native binding") != std::string::npos,
                    "an unknown handler id is a run error") &&
             ok;
        report =
            parity::run_parity(module, component, script_of("call handler::1"), std::ref(native));
        ok = expect(report.error.find("takes 1 argument") != std::string::npos,
                    "a wrong argument count is a run error") &&
             ok;
        report = parity::run_parity(module, component, script_of("call handler::1 \"x\""),
                                    std::ref(native));
        ok = expect(report.error.find("the parameter `by` is int") != std::string::npos,
                    "a wrong argument type is a run error") &&
             ok;
        const auto failing =
            [](const parity::Step*) -> std::expected<ui::StateSnapshot, std::string> {
            return std::unexpected("no such button");
        };
        report = parity::run_parity(module, component, script_of("call bind::tag"), failing);
        ok = expect(report.error.find("no such button") != std::string::npos,
                    "a native-side failure is a run error") &&
             ok;
    }

    if (!ok) {
        return 1;
    }
    std::cout << "parity_runner: ok\n";
    return 0;
}
