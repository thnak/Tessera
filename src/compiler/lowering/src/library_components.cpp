// SPDX-License-Identifier: MIT

// Built-in library components (docs/design/component-library.md) that a module may import from
// `"tessera"`. The first one is `Field` of docs/specs/forms-and-validation-v1.md section 5: a
// library component with typed prop records, a static `children` slot, and no validation logic of
// its own. Its IR is data, so codegen emits it through the ordinary component path and there is no
// intrinsic node kind or Field-specific emitter.
//
// The declared prop records are normative in docs/specs/forms-and-validation-v1.md section 5.1;
// keep the two in sync.

#include "tessera/compiler/lowering/library_components.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace tessera::compiler::lowering {
namespace {

namespace ir = tessera::compiler::ir;

constexpr std::array<LibraryComponent, 1> kLibraryComponents{
    {{.name = "Field", .id = "comp::Field"}}};

// Synthetic source spans: library IR has no project file, but every IR record carries the span its
// diagnostics point at (docs/specs/ir-v1.md section 8). The file name is stable so goldens and
// diagnostics are reproducible.
constexpr std::string_view kLibraryFile = "tessera/components/Field.tsx";
constexpr std::uint64_t kFieldComponentId = 900000000000000001ULL;
constexpr std::uint64_t kFieldNodeUiIdBase = 900000000000000101ULL;

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                    std::uint32_t column) {
    ir::SourceSpan value;
    value.file = std::string(kLibraryFile);
    value.offset = offset;
    value.length = length;
    value.line = line;
    value.column = column;
    value.end_line = line;
    value.end_column = column + length;
    return value;
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::uint32_t ordinal, std::uint32_t offset,
                 std::uint32_t line) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::to_string(static_cast<std::uint64_t>(kFieldNodeUiIdBase) + ordinal - 1);
    node.source = span(offset, 12, line, 3);
    return node;
}

ir::Prop prop_of(std::string name, std::string type, ir::ExprPtr default_value = nullptr) {
    return ir::Prop{std::move(name), std::move(type), std::move(default_value)};
}

// `Field`: label + value + visible error above the caller's widget, which arrives at the Slot.
//
// The declared props are the documented field prop records. `onValidate` is record-typed, so it is
// a callback prop bound through the node's events (`docs/specs/forms-and-validation-v1.md` section
// 5.1); everything else is a plain value record. The component computes no validation: `required`,
// the validators and the visible error are data the surrounding form owns.
ir::Component make_field() {
    ir::Component field;
    field.id = "comp::Field";
    field.component_id = std::to_string(kFieldComponentId);
    field.name = "Field";
    field.source = span(0, 400, 1, 1);
    field.props.push_back(prop_of("name", "string", ir::make_literal(std::string())));
    field.props.push_back(prop_of("label", "string", ir::make_literal(std::string())));
    field.props.push_back(prop_of("value", "string", ir::make_literal(std::string())));
    field.props.push_back(prop_of("defaultValue", "string", ir::make_literal(std::string())));
    field.props.push_back(prop_of("required", "bool", ir::make_literal(false)));
    field.props.push_back(prop_of("validators", "list<string>", ir::make_array({})));
    field.props.push_back(prop_of("onValidate", "TextChangeEvent"));
    field.props.push_back(prop_of("error", "string", ir::make_literal(std::string())));
    field.props.push_back(prop_of("errors", "list<string>", ir::make_array({})));
    field.props.push_back(prop_of("children", "children"));

    field.root = node_of(ir::NodeKind::View, "node::1", 1, 20, 3);
    field.root.props.emplace("className", ir::make_literal(std::string("field")));

    ir::Node label = node_of(ir::NodeKind::Text, "node::2", 2, 40, 4);
    label.text = ir::make_ref("prop::label");
    ir::Node value = node_of(ir::NodeKind::Text, "node::3", 3, 60, 5);
    value.text = ir::make_ref("prop::value");
    ir::Node error = node_of(ir::NodeKind::Text, "node::4", 4, 80, 6);
    error.text = ir::make_ref("prop::error");
    ir::Node slot = node_of(ir::NodeKind::Slot, "node::5", 5, 100, 7);

    field.root.children.push_back(std::move(label));
    field.root.children.push_back(std::move(value));
    field.root.children.push_back(std::move(error));
    field.root.children.push_back(std::move(slot));
    return field;
}

} // namespace

std::span<const LibraryComponent> library_components() {
    return kLibraryComponents;
}

const LibraryComponent* find_library_component(std::string_view name) {
    for (const LibraryComponent& component : kLibraryComponents) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}

ir::Component make_library_component(const LibraryComponent& component) {
    if (component.name == "Field") {
        return make_field();
    }
    // Unreachable through library_components(); keep a total answer for the encoding.
    ir::Component empty;
    empty.id = std::string(component.id);
    empty.name = std::string(component.name);
    return empty;
}

} // namespace tessera::compiler::lowering
