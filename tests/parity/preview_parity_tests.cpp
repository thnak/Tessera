// SPDX-License-Identifier: MIT
//
// Preview parity for the sample in samples/parity-demo (issue #7, docs/design/designer.md section
// 2). The sample's native handler `bind::add` has a TSX preview body and a developer C++ body (in
// the seed tests/golden/codegen/parity/Counter.cpp). This test lowers the TSX to IR, plays a script
// of calls on the IR evaluator and on the generated component, and requires the same state after
// every step.

#include "Counter.hpp"
#include "parity_runner.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef TESSERA_PARITY_SAMPLE_DIR
#error "TESSERA_PARITY_SAMPLE_DIR must name samples/parity-demo"
#endif

namespace {

namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;
namespace parity = tessera::parity;
namespace ui = tessera::ui;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "preview_parity: " << message << "\n";
    }
    return condition;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The sample's IR, lowered the way the codegen CLI does it.
std::expected<ir::IrModule, std::string> lower_sample() {
    const std::string text = read_file(std::string(TESSERA_PARITY_SAMPLE_DIR) + "/Counter.tsx");
    if (text.empty()) {
        return std::unexpected("cannot read samples/parity-demo/Counter.tsx");
    }
    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    const jsx::SourceMap map(text);
    if (parsed.root == nullptr || parsed.has_errors()) {
        return std::unexpected("the sample does not parse");
    }
    const rules::AnalyzedModule analyzed = rules::analyze_module(*parsed.root, map);
    if (analyzed.has_errors()) {
        return std::unexpected("the sample does not analyse");
    }
    const lowering::ModuleSource source{"Counter.tsx", parsed.root.get(), &analyzed, &map};
    auto lowered = lowering::lower_project(std::span<const lowering::ModuleSource>(&source, 1),
                                           "parity_sample");
    if (!lowered) {
        return std::unexpected("the sample does not lower");
    }
    ir::IrModule module;
    module.name = lowered->name;
    module.components = std::move(lowered->components);
    module.bindings = std::move(lowered->bindings);
    return module;
}

ui::Node* find_by_automation_id(ui::Node& node, std::uint64_t id) {
    if (node.automation_id() == id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (ui::Node* found = find_by_automation_id(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

// The IR node whose `onClick` is `handler`; its uiId is what the script clicks.
const ir::Node* node_with_click(const ir::Node& node, const std::string& handler) {
    const auto it = node.events.find("onClick");
    if (it != node.events.end() && it->second == handler) {
        return &node;
    }
    for (const ir::Node& child : node.children) {
        if (const ir::Node* found = node_with_click(child, handler)) {
            return found;
        }
    }
    return nullptr;
}

// The generated component with its tree mounted. A click step finds the scene node that carries
// the numeric id the compiler assigned and sends it a Click, the event a tap produces after hit
// testing.
class NativeCounter {
public:
    NativeCounter()
        : root_(std::make_unique<ui::Node>(1)) {
        counter_.build(*root_);
        counter_.mount();
    }

    std::expected<ui::StateSnapshot, std::string> operator()(const parity::Step* step) {
        if (step != nullptr) {
            if (!step->click) {
                return std::unexpected("this native side takes click steps only");
            }
            ui::Node* target = find_by_automation_id(*root_, std::stoull(step->target));
            if (target == nullptr) {
                return std::unexpected("no scene node has automation id " + step->target);
            }
            ui::Event click;
            click.type = ui::EventType::Click;
            if (!ui::dispatch_event_to(*target, click)) {
                return std::unexpected("the click on " + step->target + " reached no handler");
            }
        }
        return counter_.snapshot_state();
    }

private:
    std::unique_ptr<ui::Node> root_;
    parity_sample::gen::Counter counter_;
};

} // namespace

int main() {
    auto module = lower_sample();
    if (!expect(module.has_value(), module ? "" : module.error())) {
        return 1;
    }
    // The same salt and project-relative path the build gives the codegen CLI, so the ids here are
    // the ones in the generated component.
    const auto assigned = tessera::compiler::ids::assign_identity(*module, "parity-demo");
    if (!expect(assigned.has_value(), assigned ? "" : assigned.error().message)) {
        return 1;
    }
    const ir::Component* component = nullptr;
    for (const ir::Component& candidate : module->components) {
        if (candidate.name == "Counter") {
            component = &candidate;
        }
    }
    if (!expect(component != nullptr, "the IR has a Counter component")) {
        return 1;
    }
    bool ok = true;

    // The native handler carries a preview body; without it the comparison would prove nothing.
    bool has_preview = false;
    for (const ir::Binding& binding : module->bindings) {
        has_preview = has_preview || (binding.id == "bind::add" && binding.preview.has_value() &&
                                      binding.preview->actions.size() == 2);
    }
    ok = expect(has_preview, "bind::add has a two-action preview body") && ok;

    // Clicks by uiId, as headless-automation-v1 `click` addresses the UI: the Add button (native
    // `bind::add`) and the Reset button (the inline handler).
    const ir::Node* add_button = node_with_click(component->root, "bind::add");
    const ir::Node* reset_button = node_with_click(component->root, "handler::1");
    if (!expect(add_button != nullptr && reset_button != nullptr &&
                    add_button->ui_id != reset_button->ui_id && !add_button->ui_id.empty(),
                "both buttons have distinct uiIds")) {
        return 1;
    }
    const std::string text = "click " + add_button->ui_id + "\n" + "click " + add_button->ui_id +
                             "\n" + "click " + reset_button->ui_id + "\n" + "click " +
                             add_button->ui_id + "\n";
    const auto script = parity::parse_script(text);
    if (!expect(script.has_value(), "the script parses")) {
        return 1;
    }
    NativeCounter native;
    const parity::Report report = parity::run_parity(
        *module, *component, *script, [&](const parity::Step* step) { return native(step); });
    ok =
        expect(report.ok() && report.steps_run == 4,
               "the preview body and the developer C++ agree:\n" + parity::format_report(report)) &&
        ok;

    if (!ok) {
        return 1;
    }
    std::cout << "preview_parity: ok\n";
    return 0;
}
