// SPDX-License-Identifier: MIT
//
// Preview parity for the sample in samples/parity-demo (issue #7, docs/design/designer.md section
// 2). The sample's native handler `bind::add` has a TSX preview body and a developer C++ body (in
// the seed tests/golden/codegen/parity/Counter.cpp). This test lowers the TSX to IR, plays a script
// of calls on the IR evaluator and on the generated component, and requires the same state after
// every step.

#include "Counter.hpp"
#include "parity_runner.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"

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

void collect_buttons(ui::Node& node, std::vector<ui::Node*>& out) {
    if (node.element_kind() == "Button") {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_buttons(*child, out);
    }
}

// The generated component with its tree mounted; a step is a Click on the Button that carries the
// handler, which is the path a user's tap takes after hit testing.
class NativeCounter {
public:
    NativeCounter()
        : root_(std::make_unique<ui::Node>(1)) {
        counter_.build(*root_);
        counter_.mount();
        collect_buttons(*root_, buttons_);
    }

    std::expected<ui::StateSnapshot, std::string> operator()(const parity::Step* step) {
        if (step != nullptr) {
            const std::size_t index = button_for(step->target);
            if (index >= buttons_.size()) {
                return std::unexpected("no button carries " + step->target);
            }
            ui::Event click;
            click.type = ui::EventType::Click;
            if (!ui::dispatch_event_to(*buttons_[index], click)) {
                return std::unexpected("the click on " + step->target + " reached no handler");
            }
        }
        return counter_.snapshot_state();
    }

private:
    // Source order of the sample's two buttons: the native `add`, then the inline reset.
    static std::size_t button_for(const std::string& target) {
        if (target == "bind::add") {
            return 0;
        }
        if (target == "handler::1") {
            return 1;
        }
        return static_cast<std::size_t>(-1);
    }

    std::unique_ptr<ui::Node> root_;
    parity_sample::gen::Counter counter_;
    std::vector<ui::Node*> buttons_;
};

} // namespace

int main() {
    const auto module = lower_sample();
    if (!expect(module.has_value(), module ? "" : module.error())) {
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

    const auto script = parity::parse_script("call bind::add\n"
                                             "call bind::add\n"
                                             "call handler::1\n"
                                             "call bind::add\n");
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
