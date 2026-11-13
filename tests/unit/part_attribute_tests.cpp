// SPDX-License-Identifier: MIT
//
// The `part` attribute (issue #235, docs/specs/stylesheet-v1.md section 6.3, ADR-047 item 3): JSX
// -> IR prop -> generated `set_part`, the static-only rule, and the component name a component
// instance host carries so the host side of `::part()` can name it.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;

// The IR module the codegen library consumes. Assignment (not construction) moves the
// module-level vectors because `ir::Binding` is move-only (it owns `std::optional<Preview>`).
ir::IrModule to_module(lowering::LoweredModule& lowered) {
    ir::IrModule module;
    module.name = std::move(lowered.name);
    module.components = std::move(lowered.components);
    module.bindings = std::move(lowered.bindings);
    module.stylesheets = std::move(lowered.stylesheets);
    return module;
}

std::string generated(std::string_view path, const ir::IrModule& module) {
    const auto files = codegen::generate(module, {}, std::nullopt, codegen::EmitMode::Legacy);
    check(files.has_value(), "codegen succeeds");
    if (!files) {
        return {};
    }
    for (const codegen::GeneratedFile& file : *files) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

void test_attribute_lowers_to_a_literal_prop() {
    const auto fixture =
        parse_file("ui/App.tsx",
                   "function App() { return <View part=\"label track\"><Text>hi</Text></View>; }");
    auto lowered = lowering::lower_module(fixture->module_source(), "m");
    check(lowered.has_value(), "a module with a `part` attribute lowers");
    if (!lowered) {
        check_eq(first_code(lowered), "", "no error code");
        return;
    }
    const ir::Component* app = find_component(*lowered, "App");
    check(app != nullptr, "App is present");
    if (app == nullptr) {
        return;
    }
    const auto found = app->root.props.find("part");
    check(found != app->root.props.end(), "`part` is an IR prop of the node");
    if (found == app->root.props.end()) {
        return;
    }
    check_eq(expr_text(found->second.get()), "{\"kind\":\"literal\",\"value\":\"label track\"}",
             "the attribute lowers to a literal prop");
}

void test_dynamic_part_is_rejected() {
    // `part` is static metadata of the component source, like `id`: a computed value has no
    // meaning for a selector the engine resolves at run time.
    const auto fixture =
        parse_file("ui/App.tsx", "function App(props: P) { return <View part={props.label} />; }");
    auto lowered = lowering::lower_module(fixture->module_source(), "m");
    check(!lowered.has_value(), "a dynamic `part` is rejected");
    check_eq(first_code(lowered), "TESSERA_IR_UNSUPPORTED",
             "the rejection names the unsupported form");
    check(first_message(lowered).find("part") != std::string::npos,
          "the message names the attribute: " + first_message(lowered));
}

void test_codegen_emits_set_part() {
    const auto fixture =
        parse_file("ui/App.tsx", "function App() { return <View part=\"label\" />; }");
    auto lowered = lowering::lower_module(fixture->module_source(), "m");
    check(lowered.has_value(), "the module lowers");
    if (!lowered) {
        return;
    }
    const std::string source = generated("App.cpp", to_module(*lowered));
    check(source.find("->set_part(\"label\");") != std::string::npos,
          "codegen emits set_part for the part attribute");
}

void test_component_host_carries_its_name() {
    const auto fixture =
        parse_file("ui/App.tsx", "function Card(props: P) { return <View><Text part=\"label\">x"
                                 "</Text></View>; }\n"
                                 "function App() { return <Card />; }\n");
    auto lowered = lowering::lower_module(fixture->module_source(), "m");
    check(lowered.has_value(), "the composition module lowers");
    if (!lowered) {
        return;
    }
    ir::IrModule module = to_module(*lowered);
    const std::string page = generated("App.cpp", module);
    check(page.find("->set_component_name(\"Card\");") != std::string::npos,
          "the instance host is typed with its component name");
    const std::string card = generated("Card.cpp", module);
    check(card.find("->set_part(\"label\");") != std::string::npos,
          "the component's inner element declares its part");
}

} // namespace

int main() {
    test_attribute_lowers_to_a_literal_prop();
    test_dynamic_part_is_rejected();
    test_codegen_emits_set_part();
    test_component_host_carries_its_name();
    return finish("part_attribute");
}
