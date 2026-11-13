// SPDX-License-Identifier: MIT

// Build-time tool: writes the ADR-040 generated base of every shared IR fixture (lists, virtual
// collections, vector icons, composition, forms) into the directory given as argv[1], so the build
// compiles them against hand-written developer classes (tests/unit/codegen_base/fixtures). Never
// run by hand and never committed: it proves the reshaped output compiles for every emitter family.

#include "codegen_dynamic_module.hpp"
#include "codegen_dynamic_virtual_module.hpp"
#include "codegen_sample_module.hpp"
#include "codegen_vector_module.hpp"
#include "codegen_virtual_module.hpp"
#include "forms_codegen_fixture.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;

ir::SourceSpan span_at(std::uint32_t offset) {
    return ir::SourceSpan{"ui/Compose.tsx", offset, 10, 1, 1, 1, 11};
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id, std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span_at(offset);
    return node;
}

ir::Binding event_binding(std::string id, std::string component, std::string region,
                          std::vector<ir::Param> params, std::uint32_t offset) {
    ir::Binding binding;
    binding.id = std::move(id);
    binding.component = std::move(component);
    binding.kind = ir::BindingKind::Event;
    binding.region = std::move(region);
    binding.params = std::move(params);
    binding.source = span_at(offset);
    return binding;
}

// Composition (instances, the static Children slot, an event-bound Button) and onSizeChange
// observation (an instance forwarding to a later node), the two families the other fixtures lack.
ir::IrModule make_compose_module() {
    ir::IrModule module;
    module.name = "compose";

    ir::Component card;
    card.id = "comp::Card";
    card.component_id = "51";
    card.name = "Card";
    card.source = span_at(0);
    card.props.push_back({"children", "children", nullptr});
    card.root = node_of(ir::NodeKind::View, "node::1", "101", 10);
    card.root.children.push_back(node_of(ir::NodeKind::Slot, "node::2", "102", 20));

    ir::Component page;
    page.id = "comp::CardPage";
    page.component_id = "52";
    page.name = "CardPage";
    page.source = span_at(100);
    page.root = node_of(ir::NodeKind::View, "node::1", "201", 110);
    ir::Node instance = node_of(ir::NodeKind::Component, "node::2", "202", 120);
    instance.component = "comp::Card";
    ir::Node hello = node_of(ir::NodeKind::Text, "node::3", "203", 130);
    hello.text = ir::make_literal(std::string("hello"));
    instance.children.push_back(std::move(hello));
    ir::Node tap = node_of(ir::NodeKind::Button, "node::4", "204", 140);
    tap.props.emplace("label", ir::make_literal(std::string("tap")));
    tap.events.emplace("onClick", "bind::on_tap");
    instance.children.push_back(std::move(tap));
    page.root.children.push_back(std::move(instance));
    module.bindings.push_back(event_binding("bind::on_tap", "comp::CardPage", "on_tap", {}, 142));

    ir::Component watcher;
    watcher.id = "comp::Watcher";
    watcher.component_id = "61";
    watcher.name = "Watcher";
    watcher.source = span_at(200);
    watcher.state.push_back({"state::wide", "wide", "bool", ir::make_literal(false)});
    ir::Handler handler;
    handler.id = "handler::1";
    handler.source = span_at(220);
    handler.params.push_back({"e", "SizeChangeEvent"});
    ir::Action action;
    action.kind = ir::ActionKind::Set;
    action.target = "state::wide";
    action.value = ir::make_binary(">=", ir::make_ref("e.width"), ir::make_literal(600.0));
    handler.actions.push_back(std::move(action));
    watcher.handlers.push_back(std::move(handler));
    watcher.root = node_of(ir::NodeKind::View, "node::1", "301", 210);
    watcher.root.events.emplace("onSizeChange", "handler::1");

    ir::Component watch_page;
    watch_page.id = "comp::WatchPage";
    watch_page.component_id = "62";
    watch_page.name = "WatchPage";
    watch_page.source = span_at(300);
    watch_page.root = node_of(ir::NodeKind::View, "node::1", "401", 310);
    ir::Node watcher_instance = node_of(ir::NodeKind::Component, "node::2", "402", 320);
    watcher_instance.component = "comp::Watcher";
    watch_page.root.children.push_back(std::move(watcher_instance));
    ir::Node tail = node_of(ir::NodeKind::Text, "node::3", "403", 330);
    tail.text = ir::make_literal(std::string("tail"));
    tail.events.emplace("onSizeChange", "bind::on_tail_size");
    watch_page.root.children.push_back(std::move(tail));
    module.bindings.push_back(event_binding("bind::on_tail_size", "comp::WatchPage", "on_tail_size",
                                            {{"e", "SizeChangeEvent"}}, 335));

    module.components.push_back(std::move(card));
    module.components.push_back(std::move(page));
    module.components.push_back(std::move(watch_page));
    module.components.push_back(std::move(watcher));
    return module;
}

bool emit(const ir::IrModule& module, const std::filesystem::path& dir) {
    const auto result =
        codegen::generate(module, {}, std::nullopt, codegen::EmitMode::GeneratedBase);
    if (!result) {
        std::cerr << module.name << ": " << result.error().front() << "\n";
        return false;
    }
    for (const codegen::GeneratedFile& file : *result) {
        const std::filesystem::path path = dir / module.name / file.path;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << file.text;
    }
    return true;
}

// The sample module with its developer stubs from scaffold(): the stubs must compile against the
// generated base without any hand-written developer class (ADR-040 decision item 6).
bool emit_with_scaffold(const ir::IrModule& module, const std::filesystem::path& dir) {
    const auto base = codegen::generate(module, {}, std::nullopt, codegen::EmitMode::GeneratedBase);
    const auto stubs = codegen::scaffold(module);
    if (!base || !stubs) {
        std::cerr << module.name << ": base or scaffold failed\n";
        return false;
    }
    std::filesystem::create_directories(dir);
    for (const auto* files : {&*base, &*stubs}) {
        for (const codegen::GeneratedFile& file : *files) {
            std::ofstream(dir / file.path, std::ios::binary | std::ios::trunc) << file.text;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: codegen_base_emit_fixtures <out-dir>\n";
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    bool ok = true;
    ok = emit(tessera::test::codegen_dynamic::make_dynamic_module(), dir) && ok;
    ok = emit(tessera::test::codegen_dynamic_virtual::make_dynamic_virtual_module(), dir) && ok;
    ok = emit(tessera::test::codegen_virtual::make_virtual_module(), dir) && ok;
    ok = emit(tessera::test::codegen_vector::make_vector_module(), dir) && ok;
    ok = emit(make_compose_module(), dir) && ok;
    ok = emit_with_scaffold(tessera::test::codegen_sample::make_sample_module(),
                            dir / "scaffold_sample") &&
         ok;
    ok = emit(forms_codegen_fixture::make_forms_module(), dir) && ok;
    return ok ? 0 : 1;
}
