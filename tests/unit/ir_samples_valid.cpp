// SPDX-License-Identifier: MIT

#include "ir_samples.hpp"

#include <utility>

namespace ir_test {

ir::IrModule make_valid_module() {
    ir::IrModule module = make_rich_module();
    ir::Component& list = module.components[0];
    // The rich writer fixture places a `Slot` in the component's slot content; the valid fixture
    // replaces it with ordinary caller content (a Slot belongs in the callee, not in the caller).
    list.root.children[0].children.clear();
    ir::Node content;
    content.kind = ir::NodeKind::View;
    content.id = "node::6";
    content.ui_id = "6";
    content.source = span(240, 12, 17, 20, 17, 32);
    list.root.children[0].children.push_back(std::move(content));

    // The effect handler, its cleanup and the native handler all need a binding that is used.
    list.root.children[1].children[0].events.emplace("onTap", "bind::on_pick");

    ir::Binding sync;
    sync.id = "bind::on_sync";
    sync.component = "comp::List";
    sync.kind = ir::BindingKind::Effect;
    sync.source = span(140, 10, 11, 3, 11, 13);
    sync.region = "on_sync";
    ir::Binding cleanup = {};
    cleanup.id = "bind::on_sync_cleanup";
    cleanup.component = "comp::List";
    cleanup.kind = ir::BindingKind::Effect;
    cleanup.source = span(145, 10, 11, 8, 11, 18);
    cleanup.region = "on_sync_cleanup";
    module.bindings.push_back(std::move(sync));
    module.bindings.push_back(std::move(cleanup));

    ir::Component row;
    row.id = "comp::Row";
    row.component_id = "43";
    row.name = "Row";
    row.source = span(600, 100, 40, 1, 46, 2);
    row.props.push_back({"label", "string", nullptr});
    row.props.push_back({"tone", "string", ir::make_literal(std::string("calm"))});
    // Section 3.4: Row accepts the caller's slot content and places it once.
    row.props.push_back({"children", "children", nullptr});
    row.root.kind = ir::NodeKind::View;
    row.root.id = "node::1";
    row.root.ui_id = "10";
    row.root.source = span(620, 60, 41, 3, 45, 9);
    ir::Node slot;
    slot.kind = ir::NodeKind::Slot;
    slot.id = "node::2";
    slot.ui_id = "11";
    slot.source = span(640, 8, 43, 5, 43, 13);
    row.root.children.push_back(std::move(slot));
    module.components.push_back(std::move(row));
    return module;
}

} // namespace ir_test
