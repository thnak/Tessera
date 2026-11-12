// SPDX-License-Identifier: MIT
//
// `Path` vector-payload IR fixture for the codegen tests: one component with a `Path` that writes
// every supported prop and one that relies on the documented defaults (`fill`/`stroke` `"none"`,
// `strokeWidth` `1`). Both nodes carry a numeric `uiId` (the identity-salt scheme's output), so the
// synthesized asset names are stable across regeneration.

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace tessera::test::codegen_vector {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                           std::uint32_t column) {
    return ir::SourceSpan{"ui/VectorIcon.tsx", offset, length, line, column, line, column + length};
}

inline ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(10, 5, 1, 1);
    return node;
}

inline ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

// The explicit `Path`: every prop present, so the synthesized document carries the caller's values
// and the node gets an accessible name.
inline ir::Node explicit_path() {
    ir::Node node = leaf(ir::NodeKind::Path, "node::2", "2");
    node.props["d"] = text("M4 4 L20 4 L20 20 Z");
    node.props["viewBox"] = text("0 0 24 24");
    node.props["fill"] = text("#2e8b57");
    node.props["stroke"] = text("#102030");
    node.props["strokeWidth"] = ir::make_literal(std::int64_t{2});
    node.props["label"] = text("Logo");
    return node;
}

// The default `Path`: only the two required props, so `fill`/`stroke`/`strokeWidth` must come out
// as the spec defaults rather than being silently omitted or invented.
inline ir::Node default_path() {
    ir::Node node = leaf(ir::NodeKind::Path, "node::3", "3");
    node.props["d"] = text("M2 12 L12 2 L22 12");
    node.props["viewBox"] = text("0 0 24 24");
    return node;
}

inline ir::IrModule make_vector_module() {
    ir::IrModule module;
    module.name = "vicon";

    ir::Component component;
    component.id = "comp::VectorIcon";
    component.component_id = "1";
    component.name = "VectorIcon";
    component.source = span(0, 120, 1, 1);

    ir::Node root = leaf(ir::NodeKind::View, "node::1", "1");
    root.children.push_back(explicit_path());
    root.children.push_back(default_path());
    component.root = std::move(root);

    module.components.push_back(std::move(component));
    return module;
}

} // namespace tessera::test::codegen_vector
