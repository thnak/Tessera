// SPDX-License-Identifier: MIT

// First C++ codegen slice tests (docs/design/codegen.md): generated shape, goldens, byte-identical
// determinism (twice and on eight threads), loud rejection of unsupported constructs, and merger
// idempotency over generated output.
//
// Deliberate golden update path: `codegen_tests --write` rewrites tests/golden/codegen/*. Then
// review the diff, rebuild (the generated sample is compiled by the build), and commit.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/merger.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                    std::uint32_t column) {
    return ir::SourceSpan{"ui/Counter.tsx", offset, length, line, column, line, column + length};
}

ir::ExprPtr integer(std::int64_t value) {
    return ir::make_literal(value);
}

ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(10, 5, 1, 1);
    return node;
}

// The fixture is a valid IR module that uses every construct this codegen slice emits: state, refs,
// animated values, props, a static intrinsic tree, text, class/id/role/aria/style metadata, an
// inline handler, a native event binding, and an effect with its cleanup (region stubs only).
ir::IrModule make_sample_module() {
    ir::IrModule module;
    module.name = "demo";

    ir::Component sample;
    sample.id = "comp::Sample";
    sample.component_id = "1";
    sample.name = "Sample";
    sample.source = span(0, 100, 1, 1);
    sample.props.push_back({"title", "string", nullptr});
    sample.state.push_back({"state::count", "count", "int", integer(0)});
    sample.state.push_back({"state::name", "name", "string", ir::make_literal(std::string(""))});
    sample.refs.push_back({"ref::box", "box", "node::2"});
    sample.animated_values.push_back({"anim::opacity", "opacity", "float", ir::make_literal(1.0)});

    ir::Handler handler;
    handler.id = "handler::1";
    handler.source = span(20, 10, 2, 3);
    handler.params.push_back({"event", "ClickEvent"});
    ir::Action set;
    set.kind = ir::ActionKind::Set;
    set.target = "state::count";
    set.value = integer(1);
    handler.actions.push_back(std::move(set));
    sample.handlers.push_back(std::move(handler));

    ir::Handler change;
    change.id = "handler::2";
    change.source = span(25, 10, 3, 3);
    change.params.push_back({"e", "TextChangeEvent"});
    ir::Action set_name;
    set_name.kind = ir::ActionKind::Set;
    set_name.target = "state::name";
    set_name.value = ir::make_ref("e.value");
    change.actions.push_back(std::move(set_name));
    sample.handlers.push_back(std::move(change));

    sample.effects.push_back(
        {"effect::sync", {"state::count"}, "bind::sync", "bind::sync_cleanup", span(30, 8, 3, 3)});

    ir::Node root = leaf(ir::NodeKind::View, "node::1", "1");
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("root-"));
        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
        root.props.emplace("id", ir::make_template(std::move(parts)));
    }
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("box "));
        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
        root.props.emplace("className", ir::make_template(std::move(parts)));
    }
    root.props.emplace("role", ir::make_conditional(
                                   ir::make_binary("==", ir::make_ref("state::count"), integer(0)),
                                   text("generic"), text("status")));
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("label "));
        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
        root.props.emplace("aria-label", ir::make_template(std::move(parts)));
    }
    root.automation_id = "1";
    root.style.emplace("width", text("10px"));

    ir::Node button = leaf(ir::NodeKind::Button, "node::2", "2");
    button.props.emplace("label", text("Go"));
    button.events.emplace("onClick", "bind::on_click");
    ir::Node label = leaf(ir::NodeKind::Text, "node::3", "3");
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part("Count: "));
    parts.push_back(ir::expr_part(ir::make_ref("state::count")));
    label.text = ir::make_template(std::move(parts));
    button.children.push_back(std::move(label));
    root.children.push_back(std::move(button));

    // A second Button exercises a declared handler record (typed payload adapter).
    ir::Node handler_button = leaf(ir::NodeKind::Button, "node::4", "4");
    handler_button.events.emplace("onClick", "handler::1");
    root.children.push_back(std::move(handler_button));

    // Controlled text field: value from state, onChange is an inline setter copying the payload.
    ir::Node input = leaf(ir::NodeKind::TextInput, "node::5", "5");
    input.props.emplace("value", ir::make_ref("state::name"));
    input.events.emplace("onChange", "handler::2");
    root.children.push_back(std::move(input));
    sample.root = std::move(root);
    module.components.push_back(std::move(sample));

    for (const auto& [id, kind, region] :
         {std::tuple<std::string, ir::BindingKind, std::string>{"bind::on_click",
                                                                ir::BindingKind::Event, "on_click"},
          std::tuple<std::string, ir::BindingKind, std::string>{"bind::sync",
                                                                ir::BindingKind::Effect, "sync"},
          std::tuple<std::string, ir::BindingKind, std::string>{
              "bind::sync_cleanup", ir::BindingKind::Effect, "sync_cleanup"}}) {
        ir::Binding binding;
        binding.id = id;
        binding.component = "comp::Sample";
        binding.kind = kind;
        binding.source = span(40, 10, 4, 3);
        binding.region = region;
        if (kind == ir::BindingKind::Event) {
            binding.params.push_back({"event", "ClickEvent"});
        }
        module.bindings.push_back(std::move(binding));
    }
    return module;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string first_difference(std::string_view expected, std::string_view actual) {
    std::size_t line = 1;
    std::size_t at = 0;
    while (at < expected.size() && at < actual.size() && expected[at] == actual[at]) {
        if (expected[at] == '\n') {
            ++line;
        }
        ++at;
    }
    return "first difference at line " + std::to_string(line) + "; run `codegen_tests --write`";
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
    const auto result = codegen::generate(make_sample_module());
    check(result.has_value(), "the sample generates");
    if (!result) {
        std::cerr << "  error: " << result.error().front() << "\n";
        return;
    }
    check(result->size() == 2, "one header and one source");
    if (result->size() != 2) {
        return;
    }
    check_eq((*result)[0].path, "Sample.hpp", "deterministic header path");
    check_eq((*result)[1].path, "Sample.cpp", "deterministic source path");
    const std::string& header = (*result)[0].text;
    const std::string& source = (*result)[1].text;
    check(header.find("struct Sample") != std::string::npos, "component struct");
    check(header.find("int state_count_ = 0;") != std::string::npos, "state cell");
    check(header.find("tessera::ui::Node* ref_box_ = nullptr;") != std::string::npos, "ref field");
    check(header.find("double anim_opacity_ = 1.0;") != std::string::npos, "animated field");
    check(header.find("std::string prop_title_ = {};") != std::string::npos, "prop field");
    check(header.find("void build(tessera::ui::Node& parent);") != std::string::npos,
          "build declaration");
    check(header.find("void handler_1(const tessera::ui::ClickEvent& event);") != std::string::npos,
          "handler record signature uses the payload record");
    check(header.find("void on_click(const tessera::ui::ClickEvent& event);") != std::string::npos,
          "event binding region signature uses the payload record");
    check(header.find("#include \"tessera/ui/scene/event_payloads.hpp\"") != std::string::npos,
          "generated header includes the payload records");
    check(header.find("#include \"tessera/ui/scene/component_lifecycle.hpp\"") != std::string::npos,
          "generated header includes the lifecycle seam");
    check(header.find("tessera::ui::EffectState effect_sync_state_;") != std::string::npos,
          "one effect state per useEffect");
    check(header.find("void mount();") != std::string::npos &&
              header.find("void update_effects();") != std::string::npos &&
              header.find("void unmount();") != std::string::npos,
          "lifecycle methods are declared");
    check(header.find("void sync();") != std::string::npos, "effect binding region signature");
    check(source.find("namespace demo") != std::string::npos, "module namespace");
    check(source.find("auto node_1 = std::make_unique<tessera::ui::Node>(") != std::string::npos,
          "node creation");
    check(source.find("->set_focusable(true);") != std::string::npos, "closest existing API use");
    check(source.find("event_adapter_2_onClick") != std::string::npos &&
              source.find("event_adapter_4_onClick") != std::string::npos,
          "one adapter per event-bound node");
    check(source.find("component->on_click(tessera::ui::click_event(event));") != std::string::npos,
          "binding adapter converts the scene event to its payload record");
    check(source.find("component->handler_1(tessera::ui::click_event(event));") !=
              std::string::npos,
          "handler adapter converts the scene event to its payload record");
    check(source.find("->bind_event(tessera::ui::EventType::PointerUp, "
                      "&event_adapter_2_onClick, this);") != std::string::npos,
          "generated binding attaches with the mapped EventType");
    check(source.find("parent.add_child(std::move(node_1));") != std::string::npos,
          "root attached to the parent");
    check(source.find("->set_element_kind(\"View\");") != std::string::npos, "element kind");
    check(source.find("meta_1_id_node_ = node_1.get();") != std::string::npos,
          "dynamic id keeps a node pointer");
    check(source.find("meta_1_id_node_->set_element_id(meta_1_id_value_);") != std::string::npos,
          "dynamic id applies through set_element_id (Dirty::Style)");
    check(source.find("meta_1_class_node_->set_class_name(meta_1_class_value_);") !=
              std::string::npos,
          "dynamic className applies through set_class_name");
    check(source.find("meta_1_role_node_->set_role(meta_1_role_value_);") != std::string::npos,
          "dynamic role applies through set_role");
    check(source.find("meta_1_aria_aria_label_node_->set_aria(\"aria-label\",") !=
              std::string::npos,
          "dynamic aria applies through set_aria");
    check(source.find("->set_automation_id(1ull);") != std::string::npos, "automation id");
    check(source.find("this->text_node_3_ = node_3.get();") != std::string::npos,
          "interpolated text node pointer is stored");
    check(source.find("void Sample::apply_state()") != std::string::npos,
          "the generated apply_state recomputes interpolation");
    check(source.find("std::uint64_t hash_3 = tessera::ui::kEffectHashSeed;") != std::string::npos,
          "interpolation hashes the previous value");
    check(source.find("text_node_3_->set_paint_data(std::move(paint_3));") != std::string::npos,
          "interpolated text is written through PaintData");
    check(source.find("void Sample::update()") != std::string::npos,
          "the generated update path is emitted");
    check(source.find("state_count_ = 1;\n      apply_state();") != std::string::npos ||
              source.find("state_count_ = 1;") != std::string::npos,
          "inline setters apply the new state");
    check(source.find("style.width=\"10px\"") != std::string::npos, "remaining style gap comment");
    check(source.find("events={onClick:bind::on_click}") != std::string::npos,
          "remaining event gap comment");
    check(source.find("// [DEVELOPER_LOGIC_START: bind::on_click]") != std::string::npos,
          "on_click region");
    check(source.find("// [DEVELOPER_LOGIC_START: bind::sync]") != std::string::npos,
          "sync region");
    check(source.find("// [DEVELOPER_LOGIC_START: bind::sync_cleanup]") != std::string::npos,
          "cleanup region");
    check(
        source.find("void text_change_adapter_5(void* context, const tessera::ui::TextChangeEvent& "
                    "event)") != std::string::npos,
        "onChange uses the typed TextChange adapter");
    check(source.find("this->input_5_runtime_.attach(node_5.get());") != std::string::npos,
          "controlled field attaches its text input runtime");
    check(source.find("tessera::ui::bind_text_change(*node_5, this->input_5_change_binding_);") !=
              std::string::npos,
          "onChange binds through TextChangeBinding");
    check(source.find("state_name_ = std::string(e.value);") != std::string::npos,
          "inline setter copies the payload value");
    check(source.find("input_5_editor_.set_text(text_node_5_value_);") != std::string::npos,
          "apply_state syncs the controlled editor buffer");
    check(source.find("this->ref_box_ = node_2.get();") != std::string::npos,
          "useRef is assigned its target node");
    check(source.find("std::uint64_t dep_hash_0(const Sample& component)") != std::string::npos,
          "effect deps hash helper is generated");
    check(source.find("tessera::ui::effect_needs_run(effect_sync_state_, dep_hash_0(*this))") !=
              std::string::npos,
          "effects run through the lifecycle seam");
    check(source.find("sync_cleanup();") != std::string::npos, "effect cleanup is invoked");
    check(source.find("state_count_ = 1;") != std::string::npos, "handler setter action");
}

void test_goldens(bool update) {
    const auto result = codegen::generate(make_sample_module());
    check(result.has_value(), "the golden sample generates");
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
        if (golden != file.text) {
            check(false, file.path + ": " + first_difference(golden, file.text));
        }
    }
    if (update) {
        std::cout << "codegen goldens written to " << directory << "\n";
    }
}

std::string concatenated(const std::vector<codegen::GeneratedFile>& files) {
    std::string out;
    for (const codegen::GeneratedFile& file : files) {
        out += file.path + "\n" + file.text;
    }
    return out;
}

void test_determinism() {
    const auto first = codegen::generate(make_sample_module());
    const auto second = codegen::generate(make_sample_module());
    check(first.has_value() && second.has_value(), "the sample generates twice");
    if (first && second) {
        check_eq(concatenated(*first), concatenated(*second), "two runs are byte-identical");
    }

    constexpr std::size_t kThreads = 8;
    std::array<std::string, kThreads> texts;
    std::array<std::thread, kThreads> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&texts, i] {
            const auto generated = codegen::generate(make_sample_module());
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
        const auto result = codegen::generate(module);
        check(!result.has_value() && first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              "a conditional node is rejected loudly");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.loop = ir::Loop{"item", "index", ir::make_ref("state::rows")};
        const auto result = codegen::generate(module);
        check(!result.has_value() && first_error(result).rfind("TESSERA_CODEGEN_", 0) == 0,
              "a loop template is rejected loudly");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.kind = ir::NodeKind::Component;
        module.components[0].root.component = "comp::Other";
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a component reference is rejected until composition lands");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].state[0].type = "Row";
        const auto result = codegen::generate(module);
        check(!result.has_value() && first_error(result).find("Row") != std::string::npos,
              "a record-typed state cell has no C++ type yet");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.children[0].children[0].text = ir::make_ref("ref::box");
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a node reference has no printable text form");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.children[0].children[0].text = ir::make_literal(ir::Null{});
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a null literal has no printable text form");
    }
    {
        ir::IrModule module = make_sample_module();
        ir::Action dotted;
        dotted.kind = ir::ActionKind::Set;
        dotted.target = "state::count";
        dotted.value = ir::make_ref("event.value");
        module.components[0].handlers[0].actions.push_back(std::move(dotted));
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a record field read needs the type table");
    }
    {
        ir::IrModule module = make_sample_module();
        module.name = "9bad";
        const auto result = codegen::generate(module);
        check(!result.has_value(), "an invalid module namespace is rejected");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.kind = ir::NodeKind::CustomElement;
        module.components[0].root.element = "Waterfall3D";
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a CustomElement node is deferred");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.props["className"] = ir::make_ref("ref::box");
        const auto result = codegen::generate(module);
        check(!result.has_value(), "dynamic metadata with no printable type is rejected");
    }
    {
        ir::IrModule module = make_sample_module();
        module.components[0].root.automation_id = "not-a-number";
        const auto result = codegen::generate(module);
        check(!result.has_value(), "a non-numeric automationId is rejected");
    }
}

void test_merger_idempotency() {
    const auto result = codegen::generate(make_sample_module());
    check(result.has_value(), "the sample generates for the merger test");
    if (!result) {
        return;
    }
    std::string generated;
    for (const codegen::GeneratedFile& file : *result) {
        if (file.path == "Sample.cpp") {
            generated = file.text;
        }
    }
    check(!generated.empty(), "the generated source exists");
    constexpr std::string_view kMarker = "// [DEVELOPER_LOGIC_START: bind::on_click]";
    const std::size_t marker = generated.find(kMarker);
    check(marker != std::string::npos, "the on_click region marker exists");
    if (marker == std::string::npos) {
        return;
    }
    std::string existing = generated;
    existing.insert(marker + kMarker.size(), "\n    state_count_ = 42; // developer body");
    const tessera::compiler::MergeResult first = tessera::compiler::merge(generated, existing);
    check(first.text.find("state_count_ = 42; // developer body") != std::string::npos,
          "the developer body survives the merge");
    check(first.text.find("// [DEVELOPER_LOGIC_END: bind::on_click]") != std::string::npos,
          "the region stays closed");
    const tessera::compiler::MergeResult second = tessera::compiler::merge(first.text, existing);
    check_eq(second.text, first.text, "merge(merge(g, e), e) == merge(g, e) on generated output");
}

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::string_view(argv[1]) == "--write";
    if (update) {
        test_goldens(true);
        return failures == 0 ? 0 : 1;
    }
    test_fixture_is_valid();
    test_generated_shape();
    test_goldens(false);
    test_determinism();
    test_rejections();
    test_merger_idempotency();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen tests passed\n";
    return 0;
}
