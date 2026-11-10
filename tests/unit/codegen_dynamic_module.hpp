// SPDX-License-Identifier: MIT
//
// Dynamic IR fixture for the codegen tests: conditional branches, a keyed list and dynamic
// metadata.

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace tessera::test::codegen_dynamic {

namespace ir = tessera::compiler::ir;

inline ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                           std::uint32_t column) {
    return ir::SourceSpan{"ui/Dynamic.tsx", offset, length, line, column, line, column + length};
}

inline ir::ExprPtr integer(std::int64_t value) {
    return ir::make_literal(value);
}

inline ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = span(10, 5, 1, 1);
    return node;
}

// `{item} @ {count}`: the item text depends on both the loop item and component state, so a
// content-only change (same keys, different state) is observable without a structural change.
inline ir::ExprPtr item_text() {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::expr_part(ir::make_ref("item")));
    parts.push_back(ir::text_part(" @ "));
    parts.push_back(ir::expr_part(ir::make_ref("state::count")));
    return ir::make_template(std::move(parts));
}

inline ir::IrModule make_dynamic_module() {
    ir::IrModule module;
    module.name = "dynamic";

    ir::Component component;
    component.id = "comp::Dynamic";
    component.component_id = "1";
    component.name = "Dynamic";
    component.source = span(0, 200, 1, 1);
    // A list-typed prop seeds the list state (the only IR-expressible list initial value).
    component.props.push_back({"seed", "list<string>", nullptr});
    component.state.push_back({"state::count", "count", "int", integer(0)});
    component.state.push_back(
        {"state::items", "items", "list<string>", ir::make_ref("prop::seed")});

    ir::Node root = leaf(ir::NodeKind::View, "node::1", "1");
    root.children.push_back(leaf(ir::NodeKind::Text, "node::2", "2"));
    root.children.back().text = ir::make_literal(std::string("Header"));

    // `{count > 0 && <Text>Positive</Text>}`: one conditional branch.
    ir::Node positive = leaf(ir::NodeKind::Text, "node::3", "3");
    positive.condition = ir::make_binary(">", ir::make_ref("state::count"), integer(0));
    positive.text = ir::make_literal(std::string("Positive"));
    root.children.push_back(std::move(positive));

    // Component-level interpolation next to the dynamic children.
    ir::Node counter = leaf(ir::NodeKind::Text, "node::4", "4");
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::text_part("Count: "));
        parts.push_back(ir::expr_part(ir::make_ref("state::count")));
        counter.text = ir::make_template(std::move(parts));
    }
    root.children.push_back(std::move(counter));

    // `{items.map((item) => <Text key={item}>{item} @ {count}</Text>)}`: the list template.
    ir::Node item = leaf(ir::NodeKind::Text, "node::5", "5");
    item.text = item_text();
    item.key = ir::make_ref("item");
    item.loop = ir::Loop{"item", "", ir::make_ref("state::items")};
    root.children.push_back(std::move(item));

    // `count === 0 ? <View><Text>Empty</Text></View> : <View><Text>Filled</Text></View>`: two
    // complementary sibling branches, each with a child subtree.
    ir::Node empty = leaf(ir::NodeKind::View, "node::6", "6");
    empty.condition = ir::make_binary("==", ir::make_ref("state::count"), integer(0));
    ir::Node empty_text = leaf(ir::NodeKind::Text, "node::7", "7");
    empty_text.text = ir::make_literal(std::string("Empty"));
    empty.children.push_back(std::move(empty_text));
    root.children.push_back(std::move(empty));

    ir::Node filled = leaf(ir::NodeKind::View, "node::8", "8");
    filled.condition =
        ir::make_unary("!", ir::make_binary("==", ir::make_ref("state::count"), integer(0)));
    ir::Node filled_text = leaf(ir::NodeKind::Text, "node::9", "9");
    filled_text.text = ir::make_literal(std::string("Filled"));
    filled.children.push_back(std::move(filled_text));
    root.children.push_back(std::move(filled));

    // A protected-region binding next to the dynamic children (merger idempotency coverage).
    ir::Node button = leaf(ir::NodeKind::Button, "node::10", "10");
    button.events.emplace("onClick", "bind::on_click");
    ir::Node add_text = leaf(ir::NodeKind::Text, "node::11", "11");
    add_text.text = ir::make_literal(std::string("Add"));
    button.children.push_back(std::move(add_text));
    root.children.push_back(std::move(button));

    ir::Node footer = leaf(ir::NodeKind::Text, "node::12", "12");
    footer.text = ir::make_literal(std::string("Footer"));
    root.children.push_back(std::move(footer));

    component.root = std::move(root);
    module.components.push_back(std::move(component));

    ir::Binding binding;
    binding.id = "bind::on_click";
    binding.component = "comp::Dynamic";
    binding.kind = ir::BindingKind::Event;
    binding.region = "on_click";
    binding.source = span(40, 10, 4, 3);
    binding.params.push_back({"event", "ClickEvent"});
    module.bindings.push_back(std::move(binding));
    return module;
}

} // namespace tessera::test::codegen_dynamic
