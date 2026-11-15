// SPDX-License-Identifier: MIT

// `onSizeChange` codegen (ADR-041 slice 5, docs/design/layout-observation.md section 6.2): one
// LayoutObservation member and thunk per observed node, attach/detach_layout_observers that mirror
// attach_scheduler (an owned instance forwards, in mount order), a typed SizeChangeEvent handler
// whose body can read `e.width`, byte-identical output and goldens. The fixture
// is hand-built IR so every handler form is covered without the front end.
//
// Deliberate golden update path: `codegen_size_change_tests --write` rewrites
// tests/golden/codegen/Watcher.* and WatchPage.*; review the diff and rebuild.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace ir = tessera::compiler::ir;
namespace codegen = tessera::compiler::codegen;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(const std::string& actual, const std::string& expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

ir::SourceSpan span(std::uint32_t offset, std::uint32_t line) {
    ir::SourceSpan value;
    value.file = "ui/Watch.tsx";
    value.offset = offset;
    value.length = 10;
    value.line = line;
    value.column = 3;
    value.end_line = line;
    value.end_column = 13;
    return value;
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id, std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(offset, 3);
    return node;
}

ir::Handler set_handler(std::string id, std::string target, ir::ExprPtr value,
                        std::vector<ir::Param> params, std::uint32_t offset) {
    ir::Handler handler;
    handler.id = std::move(id);
    handler.source = span(offset, 4);
    handler.params = std::move(params);
    ir::Action action;
    action.kind = ir::ActionKind::Set;
    action.target = std::move(target);
    action.value = std::move(value);
    handler.actions.push_back(std::move(action));
    return handler;
}

// Watcher: a View observing its size with an inline `setWide(e.width >= 600)` handler and a Text
// observing its size with a parameterless handler. WatchPage owns one Watcher and observes its own
// Text through a native binding, so the forwarding order (instance first, then the later node) is
// part of the output.
ir::IrModule make_module() {
    ir::IrModule module;
    module.name = "watch";

    ir::Component watcher;
    watcher.id = "comp::Watcher";
    watcher.component_id = "61";
    watcher.name = "Watcher";
    watcher.source = span(0, 1);
    watcher.state.push_back({"state::wide", "wide", "bool", ir::make_literal(false)});
    watcher.state.push_back(
        {"state::resized", "resized", "int", ir::make_literal(std::int64_t{0})});
    watcher.handlers.push_back(
        set_handler("handler::1", "state::wide",
                    ir::make_binary(">=", ir::make_ref("e.width"), ir::make_literal(600.0)),
                    {{"e", "SizeChangeEvent"}}, 20));
    watcher.handlers.push_back(
        set_handler("handler::2", "state::resized", ir::make_literal(std::int64_t{1}), {}, 30));
    watcher.root = node_of(ir::NodeKind::View, "node::1", "301", 10);
    watcher.root.events.emplace("onSizeChange", "handler::1");
    ir::Node label = node_of(ir::NodeKind::Text, "node::2", "302", 15);
    label.text = ir::make_literal(std::string("size"));
    label.events.emplace("onSizeChange", "handler::2");
    watcher.root.children.push_back(std::move(label));

    ir::Component page;
    page.id = "comp::WatchPage";
    page.component_id = "62";
    page.name = "WatchPage";
    page.source = span(100, 10);
    page.root = node_of(ir::NodeKind::View, "node::1", "401", 110);
    ir::Node instance = node_of(ir::NodeKind::Component, "node::2", "402", 120);
    instance.component = "comp::Watcher";
    page.root.children.push_back(std::move(instance));
    ir::Node tail = node_of(ir::NodeKind::Text, "node::3", "403", 130);
    tail.text = ir::make_literal(std::string("tail"));
    tail.events.emplace("onSizeChange", "bind::on_tail_size");
    page.root.children.push_back(std::move(tail));

    // IR order is by source file, then name: `WatchPage` sorts before `Watcher`.
    module.components.push_back(std::move(page));
    module.components.push_back(std::move(watcher));

    ir::Binding binding;
    binding.id = "bind::on_tail_size";
    binding.component = "comp::WatchPage";
    binding.kind = ir::BindingKind::Event;
    binding.region = "on_tail_size";
    binding.source = span(135, 12);
    binding.params.push_back({"e", "SizeChangeEvent"});
    module.bindings.push_back(std::move(binding));
    return module;
}

// The canonical ADR-040 path (issue #218): one header-only base per component.
codegen::CodegenResult generate(const ir::IrModule& module) {
    return codegen::generate(module, {}, std::nullopt);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

std::string file_text(const std::vector<codegen::GeneratedFile>& files, std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

void test_fixture_is_valid() {
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(make_module())) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the size-change fixture is a valid IR module");
}

void test_generated_shape() {
    const auto result = generate(make_module());
    check(result.has_value(), "the size-change fixture generates: " + first_error(result));
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "Watcher.g.hpp");
    const std::string source = file_text(*result, "Watcher.g.hpp");
    const std::string page_header = file_text(*result, "WatchPage.g.hpp");
    const std::string page_source = file_text(*result, "WatchPage.g.hpp");

    check(contains(header, "#include \"tessera/ui/layout/layout_observer.hpp\""),
          "an observing header includes the observation API");
    check(contains(header, "tessera::ui::layout::LayoutObserverSet* layout_observers_ = nullptr;"),
          "the component remembers the set it is attached to");
    check(contains(header, "std::optional<tessera::ui::layout::LayoutObservation> "
                           "size_observation_1_;") &&
              contains(header, "size_observation_2_;"),
          "one observation member per observed node, by pre-order ordinal");
    check(contains(header, "void attach_layout_observers(") &&
              contains(header, "void detach_layout_observers("),
          "attach and detach mirror attach_scheduler");
    check(contains(header, "void handler_1(const tessera::ui::SizeChangeEvent& e);"),
          "a typed handler takes the SizeChangeEvent record");

    check(contains(source, "void on_size_change_1(void* context, "
                           "const tessera::ui::layout::SizeChange& size)"),
          "the thunk receives the layout SizeChange");
    check(contains(source, "component->handler_1(tessera::ui::SizeChangeEvent{size.width, "
                           "size.height, size.content_width, size.content_height});"),
          "the thunk converts the delivered size to the typed payload");
    check(contains(source, "component->handler_2(tessera::ui::Event{});"),
          "a parameterless handler needs no payload");
    check(contains(source, "this->size_observation_1_.emplace(*node_1, "
                           "&Thunks<Self>::on_size_change_1, this);"),
          "build() constructs the observation once the node exists");
    check(contains(source, "this->layout_observers_->attach(*this->size_observation_1_);"),
          "build() attaches to a set the component is already attached to");
    check(contains(source, "(e.width >= 600.0)") || contains(source, "e.width >= 600.0"),
          "the handler body reads the payload field");
    check(!contains(source, "bind_event(tessera::ui::EventType::") ||
              !contains(source, "onSizeChange"),
          "onSizeChange is not bound as a scene event");

    // Mount order: the owned instance (ordinal 2) forwards before the later node (ordinal 3).
    const std::size_t instance_attach =
        page_source.find("child_2_.attach_layout_observers(observers);");
    const std::size_t tail_attach = page_source.find("observers.attach(*size_observation_3_);");
    check(instance_attach != std::string::npos && tail_attach != std::string::npos &&
              instance_attach < tail_attach,
          "attach forwards to the instance first, in mount order");
    const std::size_t tail_detach = page_source.find("observers.detach(*size_observation_3_);");
    const std::size_t instance_detach =
        page_source.find("child_2_.detach_layout_observers(observers);");
    check(tail_detach != std::string::npos && instance_detach != std::string::npos &&
              tail_detach < instance_detach,
          "detach runs in reverse mount order");
    check(contains(page_header, "#include \"tessera/ui/layout/layout_observer.hpp\""),
          "a component that only owns an observing instance also gets the API");
}

void test_unobserved_components_are_unchanged() {
    ir::IrModule module = make_module();
    module.components[1].root.events.clear();
    module.components[1].root.children[0].events.clear();
    module.components[0].root.children[1].events.clear();
    module.bindings.clear();
    const auto result = generate(module);
    check(result.has_value(), "the fixture without observations generates: " + first_error(result));
    if (!result) {
        return;
    }
    for (const codegen::GeneratedFile& file : *result) {
        check(!contains(file.text, "layout_observer") && !contains(file.text, "size_observation"),
              "a component without onSizeChange gets no observation surface (" + file.path + ")");
    }
}

void test_goldens(bool update) {
    const auto result = generate(make_module());
    check(result.has_value(), "the size-change golden sample generates");
    if (!result) {
        return;
    }
    const std::string directory = TESSERA_CODEGEN_GOLDEN_DIR;
    if (update) {
        std::filesystem::create_directories(directory);
    }
    for (const codegen::GeneratedFile& file : *result) {
        const std::string path = directory + "/" + file.path;
        if (update) {
            write_file(path, file.text);
            continue;
        }
        const std::string golden = read_file(path);
        check(!golden.empty(), "golden " + file.path + " is readable");
        check(golden == file.text,
              file.path + " differs from its golden; run `codegen_size_change_tests --write`");
    }
    if (update) {
        std::cout << "size-change goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = generate(make_module());
    const auto second = generate(make_module());
    check(first.has_value() && second.has_value(), "the sample generates twice");
    if (!first || !second) {
        return;
    }
    check_eq(file_text(*first, "WatchPage.g.hpp"), file_text(*second, "WatchPage.g.hpp"),
             "two runs are byte-identical");
    check_eq(file_text(*first, "Watcher.g.hpp"), file_text(*second, "Watcher.g.hpp"),
             "two header runs are byte-identical");
}

void test_rejections() {
    {
        ir::IrModule module = make_module();
        module.components[1].handlers[0].params[0].type = "ClickEvent";
        const auto result = generate(module);
        check(!result.has_value(), "onSizeChange with a ClickEvent handler is rejected");
        check(contains(first_error(result), "SizeChangeEvent"),
              "the wrong payload record is named in the diagnostic");
    }
    {
        ir::IrModule module = make_module();
        module.components[1].root.events["onSizeChange"] = "handler::99";
        check(!generate(module).has_value(), "an unknown handler is rejected");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--write") {
        test_goldens(true);
        return failures == 0 ? 0 : 1;
    }
    test_fixture_is_valid();
    test_generated_shape();
    test_unobserved_components_are_unchanged();
    test_goldens(false);
    test_determinism();
    test_rejections();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
