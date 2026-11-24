// SPDX-License-Identifier: MIT

// The generated C++ of the static context form (ADR-066 item 5, issue #802): the consumer's
// environment field starts at the `createContext` default, its setter moves it (ungated for a
// record: ADR-051 generates no equality in this slice), and the providing component's
// `apply_props()` pushes its provider value into the consuming instance. The rules/lowering half
// is `lowering_context_tests`.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <string_view>

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

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length) {
    ir::SourceSpan value;
    value.file = "ui/Themed.tsx";
    value.offset = offset;
    value.length = length;
    return value;
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id, std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(offset, 4);
    return node;
}

// The corpus fixture's IR, hand-built: `ThemeContext` with a record default, a consumer whose
// environment entry names it, and a provider node whose value reads the provider's prop.
ir::IrModule make_module() {
    ir::IrModule module;
    module.name = "themed";

    ir::RecordType theme;
    theme.id = "type::Theme";
    theme.name = "Theme";
    theme.source = span(20, 14);
    theme.members.push_back(ir::RecordMember{"accent", "string", false});
    module.types.push_back(std::move(theme));

    ir::Component consumer;
    consumer.id = "comp::AccentLabel";
    consumer.component_id = "81";
    consumer.name = "AccentLabel";
    consumer.source = span(0, 1);
    ir::Environment env;
    env.id = "env::theme";
    env.name = "theme";
    env.provider = "context:ThemeContext";
    env.type = "Theme";
    env.source = span(30, 20);
    // The `createContext` default as the IR record value the lowering produces. Moved, because an
    // Expr holds unique_ptr members and cannot be copied.
    std::vector<ir::RecordField> default_fields;
    default_fields.push_back(ir::RecordField{"accent", ir::make_literal(std::string("blue"))});
    env.argument = std::move(*ir::make_record("Theme", std::move(default_fields)));
    consumer.environment.push_back(std::move(env));
    consumer.root = node_of(ir::NodeKind::View, "node::1", "801", 40);
    module.components.push_back(std::move(consumer));

    ir::Component provider;
    provider.id = "comp::ThemedPanel";
    provider.component_id = "82";
    provider.name = "ThemedPanel";
    provider.source = span(0, 1);
    ir::Prop accent_prop;
    accent_prop.name = "accent";
    accent_prop.type = "string";
    provider.props.push_back(std::move(accent_prop));
    provider.root = node_of(ir::NodeKind::View, "node::2", "802", 50);
    ir::Node provider_node = node_of(ir::NodeKind::View, "node::3", "803", 60);
    provider_node.provider = "ThemeContext";
    std::vector<ir::RecordField> provider_fields;
    provider_fields.push_back(ir::RecordField{"accent", ir::make_ref("prop::accent")});
    provider_node.provider_value = ir::make_record("Theme", std::move(provider_fields));
    ir::Node instance = node_of(ir::NodeKind::Component, "node::4", "804", 70);
    instance.component = "comp::AccentLabel";
    provider_node.children.push_back(std::move(instance));
    provider.root.children.push_back(std::move(provider_node));
    module.components.push_back(std::move(provider));
    return module;
}

void test_consumer_surface_and_default() {
    ir::IrModule module = make_module();
    const std::vector<ir::IrDiagnostic> problems = ir::validate(module);
    const bool valid_ir = problems.empty();
    check(valid_ir, "the module validates");
    if (!valid_ir) {
        std::cerr << "  " << problems.front().code << ": " << problems.front().message << "\n";
    }
    const auto result = codegen::generate(module, {}, std::nullopt);
    check(result.has_value(), result ? std::string() : result.error().front());
    if (!result) {
        return;
    }
    std::string all;
    for (const codegen::GeneratedFile& file : *result) {
        all += file.text;
    }
    // The default: the consumer's field starts at the createContext value.
    check(all.find("Theme env_theme_ = Theme{.accent = \"blue\"};") != std::string::npos,
          "the consumer's field starts at the createContext default");
    // The setter: a record has no generated equality, so the assignment is ungated.
    check(all.find("void AccentLabelBase<Self>::set_environment_theme(Theme value)") !=
              std::string::npos,
          "the consumer has a typed setter");
    // The push: the provider's apply_props pushes its record value into the consuming instance.
    check(all.find("set_environment_theme(Theme{.accent = prop_accent_})") != std::string::npos,
          "the provider's apply_props pushes its value into the consumer instance");
}

} // namespace

int main() {
    test_consumer_surface_and_default();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen context tests passed\n";
    return 0;
}
