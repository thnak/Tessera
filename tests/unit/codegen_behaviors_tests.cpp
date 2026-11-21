// SPDX-License-Identifier: MIT
//
// Behavior hook codegen (docs/specs/behavior-hooks-v1.md section 9, issue #641): a component that
// calls a behavior hook gets one `Instance` slot per behavior, a public
// `attach_behaviors(InputRouter&)` / `refresh_behaviors()` / `detach_behaviors()` triple the owner
// calls, and one read accessor per output. A component that calls none generates no behavior
// surface at all, so every existing golden stays byte-identical.
//
// The wiring is what #641 exists for: before it, the generated base owned behavior slots that
// nothing ever attached, so a behavior hook was inert in generated code no matter what the engine
// implemented.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif

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

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length) {
    ir::SourceSpan value;
    value.file = "ui/Pressable.tsx";
    value.offset = offset;
    value.length = length;
    value.line = 1;
    value.column = 3;
    value.end_line = 1;
    value.end_column = 3 + length;
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

// Pressable: `const press = usePressFeedback(targetRef);` on a View the ref points at, with a Text
// whose content reads `press.pressed`. The text binding is what makes an output observable: it is
// the expression the generated `apply_state()` re-evaluates when the behavior reports a change.
ir::IrModule make_module() {
    ir::IrModule module;
    module.name = "pressable";

    ir::Component component;
    component.id = "comp::Pressable";
    component.component_id = "81";
    component.name = "Pressable";
    component.source = span(0, 1);
    component.refs.push_back(ir::Ref{"ref::target", "target", "node::1", {}});
    ir::Behavior behavior;
    behavior.id = "beh::press";
    behavior.hook = "usePressFeedback";
    behavior.behavior = "press_feedback";
    behavior.targets.push_back("ref::target");
    behavior.outputs.push_back(ir::BehaviorOutput{"pressed", "bool"});
    behavior.outputs.push_back(ir::BehaviorOutput{"hovered", "bool"});
    behavior.outputs.push_back(ir::BehaviorOutput{"focused", "bool"});
    behavior.source = span(30, 40);
    component.behaviors.push_back(std::move(behavior));

    component.root = node_of(ir::NodeKind::View, "node::1", "701", 50);
    ir::Node label = node_of(ir::NodeKind::Text, "node::2", "702", 60);
    label.text = ir::make_ref("beh::press.pressed");
    component.root.children.push_back(std::move(label));
    module.components.push_back(std::move(component));
    return module;
}

// A component that calls no behavior hook: the control for "no surface, no golden churn".
ir::IrModule make_plain_module() {
    ir::IrModule module;
    module.name = "plain";
    ir::Component component;
    component.id = "comp::Plain";
    component.component_id = "82";
    component.name = "Plain";
    component.source = span(0, 1);
    component.root = node_of(ir::NodeKind::View, "node::1", "801", 10);
    module.components.push_back(std::move(component));
    return module;
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
    check(codes.empty(), "the behavior fixture is a valid IR module: " + codes);
}

void test_generated_surface() {
    const auto result = codegen::generate(make_module());
    check(result.has_value(), "the behavior fixture generates: " + first_error(result));
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "Pressable.g.hpp");
    check(!header.empty(), "the base header is generated");
    // The slot and the options the refresh pass reuses. The slot is the shared `Instance`, so the
    // generated base and the preview session drive a behavior the same way.
    check(contains(header, "tessera::ui::behavior::Instance beh_press_;"),
          "one behavior slot per behavior, named from its IR id");
    check(contains(header, "std::vector<tessera::ui::behavior::ArgValue> args_beh_press_;"),
          "the evaluated options are kept for the refresh pass");
    // The public seam the owner calls. Without these the slot is never attached (#641).
    check(contains(header, "void attach_behaviors(tessera::ui::InputRouter& router);"),
          "the owner attaches the behaviors through the router");
    check(contains(header, "void refresh_behaviors();"),
          "the owner refreshes the outputs after a dispatch");
    check(contains(header, "void detach_behaviors();"),
          "the owner detaches the behaviors before the tree goes away");
    // The attach body names the registry entry and the target ref.
    check(contains(header, "beh_press_.attach(\"press_feedback\""),
          "the attach call names the engine registry entry");
    check(contains(header, "targets_beh_press_"), "the attach call passes the target nodes");
    check(contains(header, "tessera::ui::behavior::Services services{&router};"),
          "the router is the one service a behavior reads");
    // The refresh pass is hash-free, which is the spec's second trigger.
    check(contains(header, "beh_press_.refresh("), "refresh recomputes without the options hash");
    // The detach pass.
    check(contains(header, "beh_press_.detach();"), "detach releases the engine behavior");
    // One read accessor per output, which is what a lowered `press.pressed` reads.
    check(contains(header, "beh_press_pressed() const"), "an output has a read accessor");
    check(contains(header, "beh_press_hovered() const"), "every output has a read accessor");
    check(contains(header, "beh_press_focused() const"), "every output has a read accessor");
    // The includes the surface needs.
    check(contains(header, "#include \"tessera/ui/behavior/instance.hpp\""),
          "the base includes the behavior instance");
    check(contains(header, "#include \"tessera/ui/scene/input_dispatch.hpp\""),
          "the base includes the router it attaches through");
}

// `useMergedRefs` (ADR-064 item 7, issue #630): one `ref` attribute fills every owner the merged
// ref names, so the generated base writes the pointer into each of them.
void test_merged_ref_assignment() {
    ir::IrModule module = make_module();
    ir::Ref merged{"ref::target", "target", "node::1"};
    merged.aliases.push_back("ref::forwarded");
    module.components[0].refs.clear();
    module.components[0].refs.push_back(std::move(merged));
    const auto result = codegen::generate(module);
    check(result.has_value(), "the merged-ref fixture generates: " + first_error(result));
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "Pressable.g.hpp");
    check(contains(header, "this->ref_target_ = "), "the primary ref is assigned");
    check(contains(header, "this->ref_forwarded_ = "),
          "the alias is assigned from the same node, so one ref attribute fills both owners");
}

void test_no_surface_without_a_hook() {
    const auto result = codegen::generate(make_plain_module());
    check(result.has_value(), "the plain fixture generates: " + first_error(result));
    if (!result) {
        return;
    }
    const std::string header = file_text(*result, "Plain.g.hpp");
    check(!header.empty(), "the plain base header is generated");
    check(!contains(header, "attach_behaviors"),
          "a component that calls no behavior hook gets no behavior surface");
    check(!contains(header, "behavior::"),
          "a component with no behavior includes no behavior code");
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

// The committed golden of the generated base. It is what proves the emitted C++ is real code and
// not merely a string a test greps: `codegen_behaviors_runtime_tests` compiles this header and
// drives the attach/refresh/detach triple through a real scene.
void test_golden(bool update) {
    const auto result = codegen::generate(make_module());
    check(result.has_value(), "the behavior golden sample generates");
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
              file.path + " differs from its golden; run `codegen_behaviors_tests --write`");
    }
    if (update) {
        std::cout << "behavior goldens written to " << directory << "\n";
    }
}

void test_determinism() {
    const auto first = codegen::generate(make_module());
    const auto second = codegen::generate(make_module());
    check(first.has_value() && second.has_value(), "the sample generates twice");
    if (!first || !second) {
        return;
    }
    check(file_text(*first, "Pressable.g.hpp") == file_text(*second, "Pressable.g.hpp"),
          "two runs are byte-identical");
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    test_fixture_is_valid();
    test_generated_surface();
    test_merged_ref_assignment();
    test_no_surface_without_a_hook();
    test_golden(update);
    test_determinism();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
