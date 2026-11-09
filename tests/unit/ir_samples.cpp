// SPDX-License-Identifier: MIT

#include "ir_samples.hpp"

#include <utility>

namespace ir_test {

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                    std::uint32_t column, std::uint32_t end_line, std::uint32_t end_column) {
    return ir::SourceSpan{"ui/Counter.tsx", offset, length, line, column, end_line, end_column};
}

namespace {

ir::Node make_node(ir::NodeKind kind, std::string id, std::string ui_id, ir::SourceSpan source) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = std::move(source);
    return node;
}

} // namespace

ir::IrModule make_counter_module() {
    ir::IrModule module;
    module.name = "demo";
    module.imports = {"tessera"};

    ir::Component counter;
    counter.id = "comp::Counter";
    counter.component_id = "1234567890123456789";
    counter.name = "Counter";
    counter.source = span(0, 310, 1, 1, 14, 2);
    counter.state.push_back({"state::count", "count", "int", ir::make_literal(std::int64_t{0})});

    ir::Node root =
        make_node(ir::NodeKind::View, "node::1", "111111111111111111", span(52, 250, 3, 3, 13, 9));
    root.props.emplace("className", ir::make_literal(std::string("counter")));
    root.style.emplace("display", ir::make_literal(std::string("flex")));
    root.style.emplace("gap", ir::make_literal(std::string("8px")));

    ir::Node text =
        make_node(ir::NodeKind::Text, "node::2", "222222222222222222", span(80, 52, 4, 5, 4, 57));
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part("Count: "));
    parts.push_back(ir::expr_part(ir::make_ref("state::count")));
    text.text = ir::make_template(std::move(parts));
    root.children.push_back(std::move(text));

    ir::Node button = make_node(ir::NodeKind::Button, "node::3", "333333333333333333",
                                span(140, 60, 5, 5, 5, 65));
    button.props.emplace("label", ir::make_literal(std::string("Increment")));
    button.events.emplace("onClick", "bind::on_click");
    root.children.push_back(std::move(button));

    counter.root = std::move(root);
    module.components.push_back(std::move(counter));

    ir::Binding binding;
    binding.id = "bind::on_click";
    binding.component = "comp::Counter";
    binding.kind = ir::BindingKind::Event;
    binding.source = span(240, 15, 12, 5, 12, 20);
    binding.region = "on_click";
    module.bindings.push_back(std::move(binding));

    module.stylesheets.push_back({"css::app", "ui/app.css", "sha256:placeholder"});
    return module;
}

} // namespace ir_test
