// SPDX-License-Identifier: MIT

// Forwarding a callback prop to an event (issue #286, docs/specs/jsx-subset-v1.md section 9.5):
// `onClick={onPress}` lowers to a handler with one `callProp` action, the IR validates, the
// diagnostics name the mismatch, and the generated C++ calls the prop's `std::function` only when
// it is set.

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace ids = tessera::compiler::ids;

bool write_goldens = false;

// The generated files are committed goldens (`--write` regenerates them); the runtime test compiles
// them. They are generated from the TSX above, never hand-edited.
void check_goldens(const std::vector<codegen::GeneratedFile>& files) {
    for (const codegen::GeneratedFile& file : files) {
        const std::filesystem::path path =
            std::filesystem::path(TESSERA_CODEGEN_GOLDEN_DIR) / file.path;
        if (write_goldens) {
            std::ofstream out(path, std::ios::binary);
            out << file.text;
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        check(text.str() == file.text, "the golden " + file.path + " is current");
    }
}

lowering::ModuleLoweringResult lower_source(std::string_view text,
                                            std::unique_ptr<FileFixture>& keep) {
    keep = parse_file("ui/Wrap.tsx", std::string(text));
    return lowering::lower_module(keep->module_source(), "forward");
}

constexpr std::string_view kWrap =
    "import { useState } from \"tessera\";\n"
    "type WrapProps = {\n"
    "  onPress: () => void;\n"
    "  onText: (e: TextChangeEvent) => void;\n"
    "};\n"
    "export function ForwardWrap({ onPress, onText }: WrapProps) {\n"
    "  return (\n"
    "    <View>\n"
    "      <Button onClick={onPress}>Go</Button>\n"
    "      <TextInput value=\"x\" onChange={onText} />\n"
    "    </View>\n"
    "  );\n"
    "}\n"
    "type CounterProps = {\n"
    "  onChanged: () => void;\n"
    "  onText: (e: TextChangeEvent) => void;\n"
    "};\n"
    "export function ForwardCounter({ onChanged, onText }: CounterProps) {\n"
    "  const [n, setN] = useState<int>(0);\n"
    "  return (\n"
    "    <View>\n"
    "      <Button onClick={() => { setN(n + 1); onChanged(); }}>Add</Button>\n"
    "      <TextInput value=\"x\" onChange={(e) => onText(e)} />\n"
    "    </View>\n"
    "  );\n"
    "}\n"
    "export function ForwardOuter() {\n"
    "  const [count, setCount] = useState<int>(0);\n"
    "  return <ForwardWrap onPress={() => setCount(count + 1)} />;\n"
    "}\n";

void test_forwarding_lowers() {
    std::unique_ptr<FileFixture> keep;
    auto result = lower_source(kWrap, keep);
    check(result.has_value(), "forwarding lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* wrap = find_component(*result, "ForwardWrap");
    check(wrap != nullptr, "ForwardWrap exists");
    if (wrap == nullptr) {
        return;
    }
    check(wrap->handlers.size() == 2, "one handler per forwarded event");
    if (wrap->handlers.size() != 2) {
        return;
    }
    const ir::Handler& press = wrap->handlers[0];
    check(press.params.empty() && press.actions.size() == 1, "the press handler has one action");
    check(press.actions[0].kind == ir::ActionKind::CallProp &&
              press.actions[0].target == "prop::onPress" && !press.actions[0].value,
          "a payload-free callback is a callProp without a value");
    const ir::Handler& text = wrap->handlers[1];
    check(text.params.size() == 1 && text.params[0].type == "TextChangeEvent",
          "the payload handler takes the record");
    check(text.actions[0].kind == ir::ActionKind::CallProp &&
              text.actions[0].target == "prop::onText" && text.actions[0].value != nullptr,
          "a payload callback passes the payload on");

    const std::string json = ir::to_text(ir::to_json(*wrap));
    check(json.find("\"callProp\"") != std::string::npos, "the JSON names the action callProp");
    auto lowered = std::move(*result);
    ir::IrModule module;
    module.name = lowered.name;
    module.components = std::move(lowered.components);
    module.bindings = std::move(lowered.bindings);
    check(ids::assign_identity(module, "forward").has_value(), "identity assigns");
    const auto diagnostics = ir::validate(module);
    check(diagnostics.empty(),
          "the module validates: " +
              (diagnostics.empty() ? std::string() : diagnostics.front().message));

    const auto generated = codegen::generate(module);
    check(generated.has_value(),
          "the module generates C++: " +
              (generated || generated.error().empty() ? std::string() : generated.error().front()));
    if (generated) {
        std::string source;
        for (const codegen::GeneratedFile& file : *generated) {
            source += file.text;
        }
        check_goldens(*generated);
        check(source.find("if (prop_onPress_)") != std::string::npos,
              "the generated handler guards an unset callback");
        check(source.find("(e);") != std::string::npos, "the payload is passed to the callback");
    }
}

void test_rejections() {
    const std::string wrong_payload =
        "type P = { onPress: (e: SizeChangeEvent) => void };\n"
        "export function A({ onPress }: P) { return <Button onClick={onPress}>x</Button>; }\n";
    std::unique_ptr<FileFixture> keep;
    auto result = lower_source(wrong_payload, keep);
    check(!result.has_value(), "a payload that is not the event's is rejected");
    check(first_code(result) == "TESSERA_IR_UNSUPPORTED", "the payload mismatch code");
    check(first_message(result).find("SizeChangeEvent") != std::string::npos,
          "the message names the record");

    const std::string not_callback =
        "type P = { label: string };\n"
        "export function A({ label }: P) { return <Button onClick={label}>x</Button>; }\n";
    result = lower_source(not_callback, keep);
    check(!result.has_value(), "a value prop is not a handler");
    check(first_code(result) == "TESSERA_IR_UNSUPPORTED", "the value prop code");

    const std::string payload_to_free_event =
        "type P = { onChange: TextChangeEvent };\n"
        "export function A({ onChange }: P) { return <Button onClick={onChange}>x</Button>; }\n";
    result = lower_source(payload_to_free_event, keep);
    check(!result.has_value(), "a payload callback on an event without that payload is rejected");

    const std::string unknown_name =
        "type P = { label: string };\n"
        "export function A({ label }: P) { return <Button onClick={nope}>{label}</Button>; }\n";
    keep = parse_file_raw("ui/Wrap.tsx", unknown_name);
    result = lowering::lower_module(keep->module_source(), "forward");
    check(!result.has_value(), "an unknown name is still unresolved");
    check(first_code(result) == "TESSERA_IR_UNRESOLVED", "the unresolved code");
}

void test_calls_from_a_handler_body() {
    std::unique_ptr<FileFixture> keep;
    auto result = lower_source(kWrap, keep);
    check(result.has_value(), "calls lower: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* counter = find_component(*result, "ForwardCounter");
    check(counter != nullptr && counter->handlers.size() == 2, "the counter has two handlers");
    if (counter == nullptr || counter->handlers.size() != 2) {
        return;
    }
    const auto& add = counter->handlers[0].actions;
    check(add.size() == 2 && add[0].kind == ir::ActionKind::Set &&
              add[1].kind == ir::ActionKind::CallProp && add[1].target == "prop::onChanged" &&
              !add[1].value,
          "the setter runs first, then the callback is called");
    const auto& text = counter->handlers[1];
    check(text.params.size() == 1 && text.actions.size() == 1 &&
              text.actions[0].kind == ir::ActionKind::CallProp && text.actions[0].value != nullptr,
          "a payload callback is called with the handler's own parameter");
}

void test_call_rejections() {
    std::unique_ptr<FileFixture> keep;
    const std::string with_argument = "type P = { onPress: () => void };\n"
                                      "export function A({ onPress }: P) {\n"
                                      "  return <Button onClick={(e) => onPress(e)}>x</Button>;\n"
                                      "}\n";
    auto result = lower_source(with_argument, keep);
    check(!result.has_value(), "a payload-free callback takes no argument");
    check(first_code(result) == "TESSERA_IR_UNSUPPORTED", "the argument code");

    const std::string missing_argument =
        "type P = { onText: (e: TextChangeEvent) => void };\n"
        "export function A({ onText }: P) {\n"
        "  return <TextInput value=\"x\" onChange={() => onText()} />;\n"
        "}\n";
    result = lower_source(missing_argument, keep);
    check(!result.has_value(), "a payload callback needs its payload");
    check(first_message(result).find("TextChangeEvent") != std::string::npos,
          "the message names the record");

    const std::string wrong_record =
        "type P = { onText: (e: SizeChangeEvent) => void };\n"
        "export function A({ onText }: P) {\n"
        "  return <TextInput value=\"x\" onChange={(e) => onText(e)} />;\n"
        "}\n";
    result = lower_source(wrong_record, keep);
    check(!result.has_value(), "the payload record must match the handler parameter");

    const std::string expression_argument =
        "type P = { onText: (e: TextChangeEvent) => void };\n"
        "export function A({ onText }: P) {\n"
        "  return <TextInput value=\"x\" onChange={(e) => onText(e.value)} />;\n"
        "}\n";
    result = lower_source(expression_argument, keep);
    check(!result.has_value(), "only the whole payload parameter is passed on");

    const std::string value_prop = "type P = { label: string };\n"
                                   "export function A({ label }: P) {\n"
                                   "  return <Button onClick={() => label()}>x</Button>;\n"
                                   "}\n";
    result = lower_source(value_prop, keep);
    check(!result.has_value(), "a value prop cannot be called");
}

void test_forward_to_child_component() {
    const std::string source = "type InnerProps = { onPress: () => void };\n"
                               "export function Inner({ onPress }: InnerProps) {\n"
                               "  return <Button onClick={onPress}>go</Button>;\n"
                               "}\n"
                               "type OuterProps = { onPress: () => void };\n"
                               "export function Outer({ onPress }: OuterProps) {\n"
                               "  return <Inner onPress={onPress} />;\n"
                               "}\n";
    std::unique_ptr<FileFixture> keep;
    const auto result = lower_source(source, keep);
    check(result.has_value(), "a callback prop is handed on to a child: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* outer = find_component(*result, "Outer");
    check(outer != nullptr && outer->handlers.size() == 1 &&
              outer->handlers[0].actions[0].kind == ir::ActionKind::CallProp,
          "the child's callback is bound to a callProp handler");
}

} // namespace

int main(int argc, char** argv) {
    write_goldens = argc > 1 && std::string_view(argv[1]) == "--write";
    test_forwarding_lowers();
    test_rejections();
    test_forward_to_child_component();
    test_calls_from_a_handler_body();
    test_call_rejections();
    return jsx_test::finish("lowering callback forward");
}
