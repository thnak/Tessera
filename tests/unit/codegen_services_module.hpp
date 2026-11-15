// SPDX-License-Identifier: MIT

// Hand-built IR for the `useService<T>()` fixture (ADR-050 section 3.1, AF5, issue #177). Shared by
// the emission test (which owns the stored golden through `--write`) and the runtime test that
// compiles it and asserts the resolve cost.
//
// `Inbox` declares two requirements, which is the shape ADR-050 section 3 measures ("a component
// that needs 8 services costs about 60 ns" at mount) in miniature:
//   const nav = useService<demo::Navigator>();
//   const store = useService<demo::MailStore>();
// and its root node is a Button whose click binding reaches them:
//   <Button onClick={bind::open} />
//
// The point of the fixture is that `Inbox` resolves each service exactly once, in `mount()`, and
// that `service<T>()` for a `T` outside this list is a compile error.

#pragma once

#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace tessera::test::codegen_services {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t line, std::uint32_t column) {
    return ir::SourceSpan{"ui/Inbox.tsx", offset, 10, line, column, line, column + 10};
}

inline ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id, std::uint32_t offset) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(offset, 1, 1);
    return node;
}

inline ir::IrModule make_services_module() {
    ir::IrModule module;
    module.name = "demo";

    ir::Component inbox;
    inbox.id = "comp::Inbox";
    inbox.component_id = "71";
    inbox.name = "Inbox";
    inbox.source = span(0, 1, 1);
    // In source order, exactly as the TSX reads. Both entries name a real project header, which is
    // what the manifest reader of a later slice fills in.
    inbox.services.push_back(
        {"svc::nav", "nav", "demo::Navigator", "demo/services.hpp", span(20, 3, 15)});
    inbox.services.push_back(
        {"svc::store", "store", "demo::MailStore", "demo/services.hpp", span(60, 4, 15)});

    ir::Node root = leaf(ir::NodeKind::Button, "node::1", "701", 100);
    root.props.emplace("label", ir::make_literal(std::string("Open")));
    root.events.emplace("onClick", "bind::open");
    inbox.root = std::move(root);
    module.components.push_back(std::move(inbox));

    ir::Binding open;
    open.id = "bind::open";
    open.component = "comp::Inbox";
    open.kind = ir::BindingKind::Event;
    open.source = span(110, 4, 20);
    open.region = "open";
    module.bindings.push_back(std::move(open));
    return module;
}

// A second component in the same module with no `useService` at all: the control case that proves
// the service surface is emitted per component, not per module.
inline ir::IrModule make_mixed_module() {
    ir::IrModule module = make_services_module();
    ir::Component plain;
    plain.id = "comp::Plain";
    plain.component_id = "72";
    plain.name = "Plain";
    plain.source = span(200, 1, 1);
    plain.root = leaf(ir::NodeKind::View, "node::1", "801", 210);
    module.components.push_back(std::move(plain));
    return module;
}

} // namespace tessera::test::codegen_services
