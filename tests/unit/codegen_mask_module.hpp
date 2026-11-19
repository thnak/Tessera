// SPDX-License-Identifier: MIT
//
// IR fixture for the ADR-056 dirty-mask tests: one component whose bindings each read a different,
// known set of cells, so a test can mutate one cell and name exactly the bindings that must re-run.
//
//   node::2 text      `A:{a}`                 reads state::a
//   node::3 text      `B:{b}`                 reads state::b
//   node::4 text      `T:{title}`             reads prop::title
//   node::5 view      className `row-{b}`     reads state::b
//                     style color = tone      reads state::tone
//   node::6 text      shown when `flag`       reads state::flag
//   node::7 list      `{item}/{a}` over items reads state::items and state::a

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::codegen_mask {

namespace ir = tessera::compiler::ir;

inline ir::Node leaf(ir::NodeKind kind, std::string id, std::string ui_id) {
    ir::Node node;
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    node.source = ir::SourceSpan{"ui/Probe.tsx", 10, 5, 1, 1, 1, 6};
    return node;
}

inline ir::ExprPtr label(std::string prefix, const char* ref) {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part(std::move(prefix)));
    parts.push_back(ir::expr_part(ir::make_ref(ref)));
    return ir::make_template(std::move(parts));
}

inline ir::IrModule make_mask_module() {
    ir::IrModule module;
    module.name = "demo";

    ir::Component component;
    component.id = "comp::Probe";
    component.component_id = "1";
    component.name = "Probe";
    component.source = ir::SourceSpan{"ui/Probe.tsx", 0, 100, 1, 1, 1, 101};
    component.props.push_back({"title", "string", ir::make_literal(std::string(""))});
    component.state.push_back({"state::a", "a", "int", ir::make_literal(std::int64_t{0})});
    component.state.push_back({"state::b", "b", "int", ir::make_literal(std::int64_t{0})});
    component.state.push_back(
        {"state::tone", "tone", "string", ir::make_literal(std::string("red"))});
    component.state.push_back({"state::flag", "flag", "bool", ir::make_literal(false)});
    component.state.push_back(
        {"state::items", "items", "list<string>", ir::make_array(std::vector<ir::ExprPtr>{})});

    ir::Node root = leaf(ir::NodeKind::View, "node::1", "1");

    ir::Node text_a = leaf(ir::NodeKind::Text, "node::2", "2");
    text_a.text = label("A:", "state::a");
    root.children.push_back(std::move(text_a));

    ir::Node text_b = leaf(ir::NodeKind::Text, "node::3", "3");
    text_b.text = label("B:", "state::b");
    root.children.push_back(std::move(text_b));

    ir::Node text_title = leaf(ir::NodeKind::Text, "node::4", "4");
    text_title.text = label("T:", "prop::title");
    root.children.push_back(std::move(text_title));

    ir::Node styled = leaf(ir::NodeKind::View, "node::5", "5");
    styled.props.emplace("className", label("row-", "state::b"));
    styled.style.emplace("color", ir::make_ref("state::tone"));
    root.children.push_back(std::move(styled));

    ir::Node shown = leaf(ir::NodeKind::Text, "node::6", "6");
    shown.condition = ir::make_ref("state::flag");
    shown.text = ir::make_literal(std::string("Flag"));
    root.children.push_back(std::move(shown));

    ir::Node item = leaf(ir::NodeKind::Text, "node::7", "7");
    {
        std::vector<ir::TemplatePart> parts;
        parts.push_back(ir::expr_part(ir::make_ref("item")));
        parts.push_back(ir::text_part("/"));
        parts.push_back(ir::expr_part(ir::make_ref("state::a")));
        item.text = ir::make_template(std::move(parts));
    }
    item.key = ir::make_ref("item");
    item.loop = ir::Loop{"item", "", ir::make_ref("state::items")};
    root.children.push_back(std::move(item));

    component.root = std::move(root);
    module.components.push_back(std::move(component));
    return module;
}

} // namespace tessera::test::codegen_mask
