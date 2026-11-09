// SPDX-License-Identifier: MIT

#include "ir_samples.hpp"

#include <utility>

namespace ir_test {
namespace {

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id, ir::SourceSpan source) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = std::move(source);
    return node;
}

ir::ExprPtr integer(std::int64_t value) {
    return ir::make_literal(value);
}

ir::ExprPtr text(std::string value) {
    return ir::make_literal(std::move(value));
}

ir::Handler make_handler() {
    ir::Handler handler;
    handler.id = "handler::1";
    handler.source = span(100, 40, 9, 5, 9, 45);
    handler.params.push_back({"e", "string"});
    ir::Action add;
    add.kind = ir::ActionKind::Set;
    add.target = "state::n";
    add.value = ir::make_binary("+", ir::make_ref("state::n"), integer(1));
    handler.actions.push_back(std::move(add));
    ir::Action fade;
    fade.kind = ir::ActionKind::AnimSet;
    fade.target = "anim::fade";
    fade.value = ir::make_literal(0.5);
    fade.animate = false;
    handler.actions.push_back(std::move(fade));
    return handler;
}

ir::Node make_root() {
    ir::Node root = node_of(ir::NodeKind::View, "node::1", "1", span(200, 250, 16, 3, 28, 9));

    ir::Node row = node_of(ir::NodeKind::Component, "node::2", "2", span(210, 60, 17, 5, 17, 65));
    row.component = "comp::Row";
    row.key = ir::make_ref("item.id");
    row.props.emplace("label", ir::make_ref("item.label"));
    row.props.emplace("tone", text("calm"));
    row.events.emplace("onPick", "handler::1");
    row.condition = ir::make_unary("!", ir::make_ref("prop::title"));
    row.loop = ir::Loop{"item", "index", ir::make_ref("state::rows")};
    ir::Node slot = node_of(ir::NodeKind::Text, "node::6", "6", span(240, 12, 17, 20, 17, 32));
    slot.text = text("slot");
    row.children.push_back(std::move(slot));
    root.children.push_back(std::move(row));

    root.props.emplace("className", text("root"));
    root.style.emplace("width", text("10px"));
    root.style.emplace("opacity", ir::make_literal(0.5));

    ir::Node boundary =
        node_of(ir::NodeKind::ErrorBoundary, "node::3", "3", span(280, 100, 19, 5, 25, 7));
    boundary.automation_id = "3";
    ir::Node label = node_of(ir::NodeKind::Text, "node::4", "4", span(290, 20, 20, 7, 20, 27));
    std::vector<ir::ExprPtr> args;
    args.push_back(ir::make_ref("state::n"));
    args.push_back(integer(2));
    label.text = ir::make_conditional(ir::make_ref("prop::title"),
                                      ir::make_call("formatNumber", std::move(args)),
                                      ir::make_literal(ir::Null{}));
    boundary.children.push_back(std::move(label));
    boundary.fallback = std::make_unique<ir::Node>(
        node_of(ir::NodeKind::View, "node::5", "5", span(300, 10, 22, 7, 22, 17)));
    root.children.push_back(std::move(boundary));
    return root;
}

} // namespace

ir::IrModule make_rich_module() {
    ir::IrModule module;
    module.name = "demo";

    ir::Component list;
    list.id = "comp::List";
    list.component_id = "42";
    list.name = "List";
    list.source = span(0, 500, 1, 1, 30, 2);
    list.props.push_back({"start", "int", integer(1)});
    list.props.push_back({"title", "string", nullptr});
    list.state.push_back({"state::rows", "rows", "list<int>", ir::make_ref("prop::start")});
    list.state.push_back({"state::n", "n", "float", ir::make_literal(1.0)});
    list.handlers.push_back(make_handler());
    list.effects.push_back({"effect::sync",
                            {"state::n"},
                            "bind::on_sync",
                            "bind::on_sync_cleanup",
                            span(150, 60, 12, 5, 14, 8)});
    list.refs.push_back({"ref::box", "box", "node::1"});
    list.animated_values.push_back({"anim::fade", "fade", "float", ir::make_literal(1.0)});
    list.environment.push_back(
        {"env::theme", "theme", "theme", "string", true, span(20, 30, 3, 3, 3, 33)});
    list.root = make_root();
    module.components.push_back(std::move(list));

    ir::Binding pick;
    pick.id = "bind::on_pick";
    pick.component = "comp::List";
    pick.kind = ir::BindingKind::Event;
    pick.source = span(60, 18, 7, 3, 7, 21);
    pick.region = "on_pick";
    pick.params = {{"id", "int"}, {"label", "string"}};
    pick.return_type = "bool";
    ir::Preview preview;
    ir::Action set;
    set.target = "state::n";
    set.value = text(std::string("a\"b\\c\n\x01\xC3\xA9"));
    preview.actions.push_back(std::move(set));
    preview.return_value = ir::make_literal(true);
    pick.preview = std::move(preview);
    module.bindings.push_back(std::move(pick));
    return module;
}

} // namespace ir_test
