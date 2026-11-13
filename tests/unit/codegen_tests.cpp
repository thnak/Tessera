// SPDX-License-Identifier: MIT

// Codegen tests on the ADR-040 generated base (docs/design/codegen.md): the shape of the emitted
// `Sample.g.hpp`, byte-identical determinism (twice and on eight threads) and loud rejection of
// unsupported constructs. The golden of the base is owned by codegen_base_tests
// (`codegen_base_tests --write`), so this file never writes a golden.

#include "codegen_sample_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

using tessera::test::codegen_sample::make_sample_module;

codegen::CodegenResult generate_base(const ir::IrModule& module) {
    return codegen::generate(module, {}, std::nullopt, codegen::EmitMode::GeneratedBase);
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

void test_fixture_is_valid() {
    const ir::IrModule module = make_sample_module();
    const std::vector<ir::IrDiagnostic> diagnostics = ir::validate(module);
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : diagnostics) {
        codes += diagnostic.code + ":" + diagnostic.message + "; ";
    }
    check_eq(codes, "", "the codegen fixture is a valid IR module");
}

void test_generated_shape() {
    const auto result = generate_base(make_sample_module());
    check(result.has_value(), "the sample generates");
    if (!result) {
        std::cerr << "  error: " << result.error().front() << "\n";
        return;
    }
    check(result->size() == 1, "one header-only file per component");
    if (result->size() != 1) {
        return;
    }
    check_eq((*result)[0].path, "Sample.g.hpp", "deterministic generated-base path");
    const std::string& text = (*result)[0].text;
    check(contains(text, "struct SampleBase"), "component base struct");
    check(contains(text, "int state_count_ = 0;"), "state cell");
    check(contains(text, "tessera::ui::Node* ref_box_ = nullptr;"), "ref field");
    check(contains(text, "double anim_opacity_ = 1.0;"), "animated field");
    check(contains(text, "std::string prop_title_ = {};"), "prop field");
    check(contains(text, "void build(tessera::ui::Node& parent);"), "build declaration");
    check(contains(text, "void handler_1(const tessera::ui::ClickEvent& event);"),
          "handler record signature uses the payload record");
    check(contains(text, "void on_click(const tessera::ui::ClickEvent& event)"),
          "event binding thunk signature uses the payload record");
    check(contains(text, "#include \"tessera/ui/scene/event_payloads.hpp\""),
          "generated header includes the payload records");
    check(contains(text, "#include \"tessera/ui/scene/component_lifecycle.hpp\""),
          "generated header includes the lifecycle seam");
    check(contains(text, "tessera::ui::EffectState effect_sync_state_;"),
          "one effect state per useEffect");
    check(contains(text, "void mount();") && contains(text, "void update_effects();") &&
              contains(text, "void unmount();"),
          "lifecycle methods are declared");
    check(contains(text, "void sync()"), "effect binding thunk signature");
    check(contains(text, "namespace demo"), "module namespace");
    check(contains(text, "auto node_1 = std::make_unique<tessera::ui::Node>("), "node creation");
    check(contains(text, "->set_focusable(true);"), "closest existing API use");
    check(contains(text, "event_adapter_2_onClick") && contains(text, "event_adapter_4_onClick"),
          "one adapter per event-bound node");
    check(contains(text, "component->on_click(tessera::ui::click_event(event));"),
          "binding adapter converts the scene event to its payload record");
    check(contains(text, "component->handler_1(tessera::ui::click_event(event));"),
          "handler adapter converts the scene event to its payload record");
    check(contains(text, "->bind_event(tessera::ui::EventType::Click, "),
          "generated binding attaches with the mapped EventType");
    check(contains(text, "parent.add_child(std::move(node_1));"), "root attached to the parent");
    check(contains(text, "->set_element_kind(\"View\");"), "element kind");
    check(contains(text, "meta_1_id_node_ = node_1.get();"), "dynamic id keeps a node pointer");
    check(contains(text, "meta_1_id_node_->set_element_id(meta_1_id_value_);"),
          "dynamic id applies through set_element_id (Dirty::Style)");
    check(contains(text, "meta_1_class_node_->set_class_name(meta_1_class_value_);"),
          "dynamic className applies through set_class_name");
    check(contains(text, "meta_1_role_node_->set_role(meta_1_role_value_);"),
          "dynamic role applies through set_role");
    check(contains(text, "meta_1_aria_aria_label_node_->set_aria(\"aria-label\","),
          "dynamic aria applies through set_aria");
    check(contains(text, "->set_automation_id(1ull);"), "automation id");
    check(contains(text, "this->text_node_3_ = node_3.get();"),
          "interpolated text node pointer is stored");
    check(contains(text, "SampleBase<Self>::apply_state()"),
          "the generated apply_state recomputes interpolation");
    check(contains(text, "std::uint64_t hash_3 = tessera::ui::kEffectHashSeed;"),
          "interpolation hashes the previous value");
    check(contains(text, "text_node_3_->set_paint_data(std::move(paint_3));"),
          "interpolated text is written through PaintData");
    check(contains(text, "SampleBase<Self>::update()"), "the generated update path is emitted");
    check(contains(text, "->set_inline_style(\"width\", \"10px\");"),
          "static inline style is emitted as an attribute setter");
    check(contains(text, "meta_1_style_opacity_node_->set_inline_style(\"opacity\","),
          "dynamic inline style is applied through apply_state");
    check(contains(text, "events={onClick:bind::on_click}"), "remaining event gap comment");
    check(!contains(text, "[DEVELOPER_LOGIC_START"), "no protected regions in the generated base");
    check(contains(text, "SampleContract"),
          "the developer class contract is checked at compile time");
    check(contains(text, "text_change_adapter_5(void* context, const tessera::ui::TextChangeEvent& "
                         "event)"),
          "onChange uses the typed TextChange adapter");
    check(contains(text, "this->input_5_runtime_.attach(node_5.get());"),
          "controlled field attaches its text input runtime");
    check(contains(text, "tessera::ui::bind_text_change(*node_5, this->input_5_change_binding_);"),
          "onChange binds through TextChangeBinding");
    check(contains(text, "state_name_ = std::string(e.value);"),
          "inline setter copies the payload value");
    check(contains(text, "input_5_editor_.set_text(text_node_5_value_);"),
          "apply_state syncs the controlled editor buffer");
    check(contains(text, "this->ref_box_ = node_2.get();"), "useRef is assigned its target node");
    check(contains(text, "dep_hash_0(const SampleBase<Self>& component)"),
          "effect deps hash helper is generated");
    check(contains(
              text,
              "tessera::ui::effect_needs_run(effect_sync_state_, Thunks<Self>::dep_hash_0(*this))"),
          "effects run through the lifecycle seam");
    check(contains(text, "sync_cleanup();"), "effect cleanup is invoked");
    check(contains(text, "state_count_ = 1;"), "handler setter action");
}

std::string concatenated(const std::vector<codegen::GeneratedFile>& files) {
    std::string out;
    for (const codegen::GeneratedFile& file : files) {
        out += file.path + "\n" + file.text;
    }
    return out;
}

void test_determinism() {
    const auto first = generate_base(make_sample_module());
    const auto second = generate_base(make_sample_module());
    check(first.has_value() && second.has_value(), "the sample generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second), "two runs are byte-identical");
    }

    constexpr std::size_t kThreads = 8;
    std::array<std::string, kThreads> texts;
    std::array<std::thread, kThreads> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&texts, i] {
            const auto generated = generate_base(make_sample_module());
            texts[i] = generated ? concatenated(*generated) : "<error>";
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 1; i < kThreads; ++i) {
        check_eq(texts[i], texts[0], "concurrent codegen agrees with thread 0");
    }
}

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

void test_rejections() {
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.condition = ir::make_literal(true);
        const auto result = generate_base(module);
        check(!result.has_value() && first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              "a conditional node is rejected loudly");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.loop = ir::Loop{"item", "index", ir::make_ref("state::rows")};
        const auto result = generate_base(module);
        check(!result.has_value() && first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              "a loop template is rejected loudly");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.kind = ir::NodeKind::Component;
        module.components[0].root.component = "comp::Other";
        const auto result = generate_base(module);
        check(!result.has_value(), "a component reference is rejected until composition lands");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].state[0].type = "Row";
        const auto result = generate_base(module);
        check(!result.has_value() && first_error(result).find("Row") != std::string::npos,
              "a record-typed state cell has no C++ type yet");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.children[0].children[0].text = ir::make_ref("ref::box");
        const auto result = generate_base(module);
        check(!result.has_value(), "a node reference has no printable text form");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.children[0].children[0].text = ir::make_literal(ir::Null{});
        const auto result = generate_base(module);
        check(!result.has_value(), "a null literal has no printable text form");
    }
    {
        ir::IrModule module = make_sample_module();
        ir::Action dotted;
        dotted.kind = ir::ActionKind::Set;
        dotted.target = "state::count";
        dotted.value = ir::make_ref("event.value");
        module.components[0].handlers[0].actions.push_back(std::move(dotted));
        const auto result = generate_base(module);
        check(!result.has_value(), "a record field read needs the type table");
    }
    {
        ir::IrModule module = make_sample_module();
        module.name = "9bad";
        const auto result = generate_base(module);
        check(!result.has_value(), "an invalid module namespace is rejected");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.kind = ir::NodeKind::CustomElement;
        module.components[0].root.element = "Waterfall3D";
        const auto result = generate_base(module);
        check(!result.has_value(), "a CustomElement node is deferred");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.props["className"] = ir::make_ref("ref::box");
        const auto result = generate_base(module);
        check(!result.has_value(), "dynamic metadata with no printable type is rejected");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.automation_id = "not-a-number";
        const auto result = generate_base(module);
        check(!result.has_value(), "a non-numeric automationId is rejected");
    }
}

} // namespace

int main() {
    test_fixture_is_valid();
    test_generated_shape();
    test_determinism();
    test_rejections();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen tests passed\n";
    return 0;
}
