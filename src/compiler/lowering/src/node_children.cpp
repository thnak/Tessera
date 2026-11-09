// SPDX-License-Identifier: MIT

// Children of slices 7 and 8 (docs/specs/jsx-subset-v1.md sections 10.4 and 10.5, elements-basic.md
// section 0): fragments splice, text runs become the node `text`/`props.label` or synthetic `Text`
// children, conditional children contribute one or two conditional sibling nodes, and a `.map`
// child contributes the loop template node. The schema children rule of the element decides what
// is accepted.

#include "node_lowering_internal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

struct ChildItem {
    enum class Kind { Text, Expression, Element, Conditional, Loop };

    Kind kind = Kind::Text;
    const jsx::JsxNode* node = nullptr; // Text node, expression container or element
    const jsx::Expr* expr = nullptr;    // Expression, Conditional and Loop kinds
};

bool is_run_item(const ChildItem& item) {
    return item.kind == ChildItem::Kind::Text || item.kind == ChildItem::Kind::Expression;
}

bool flatten_children(NodeState& state, const jsx::JsxNode& parent, std::vector<ChildItem>& items,
                      bool reject_fragments) {
    for (const jsx::JsxPtr& child : parent.children) {
        if (!child) {
            fail_node(state, "TESSERA_IR_MALFORMED", "an element has a missing child", parent.span);
            return false;
        }
        switch (child->kind) {
        case jsx::JsxKind::Text:
            items.push_back(ChildItem{ChildItem::Kind::Text, child.get(), nullptr});
            break;
        case jsx::JsxKind::Fragment:
            if (reject_fragments) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "a fragment child is not allowed here (subset 10.4)", child->span);
                return false;
            }
            if (!flatten_children(state, *child, items, reject_fragments)) {
                return false;
            }
            break;
        case jsx::JsxKind::ExprContainer: {
            if (!child->expr) {
                break; // `{}` and `{/* comment */}` render nothing
            }
            const jsx::Expr& expr = *child->expr;
            if (expr.kind == jsx::ExprKind::Boolean || expr.kind == jsx::ExprKind::Null) {
                break; // renders nothing and does not break a run (elements-basic.md section 0)
            }
            const ControlKind control = classify_control(expr);
            if (control == ControlKind::Conditional) {
                items.push_back(ChildItem{ChildItem::Kind::Conditional, child.get(), &expr});
                break;
            }
            if (control == ControlKind::Loop) {
                items.push_back(ChildItem{ChildItem::Kind::Loop, child.get(), &expr});
                break;
            }
            if (expr.kind == jsx::ExprKind::Jsx) {
                if (!expr.jsx) {
                    fail_node(state, "TESSERA_IR_MALFORMED", "a JSX expression has no element",
                              expr.span);
                    return false;
                }
                if (expr.jsx->kind == jsx::JsxKind::Fragment) {
                    if (reject_fragments) {
                        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                                  "a fragment child is not allowed here (subset 10.4)",
                                  expr.jsx->span);
                        return false;
                    }
                    if (!flatten_children(state, *expr.jsx, items, reject_fragments)) {
                        return false;
                    }
                } else {
                    items.push_back(ChildItem{ChildItem::Kind::Element, expr.jsx.get(), nullptr});
                }
                break;
            }
            items.push_back(ChildItem{ChildItem::Kind::Expression, child.get(), &expr});
            break;
        }
        case jsx::JsxKind::Element:
            items.push_back(ChildItem{ChildItem::Kind::Element, child.get(), nullptr});
            break;
        }
    }
    return true;
}

jsx::Span run_span(const std::vector<const ChildItem*>& run) {
    if (run.empty()) {
        return {};
    }
    const jsx::Span& first = run.front()->node->span;
    const jsx::Span& last = run.back()->node->span;
    return jsx::Span{first.offset, last.end() - first.offset};
}

// One run: JSX text and string/number expressions with no element in between. The result is a
// literal string when there is no expression, otherwise a template (elements-basic.md section 0).
ir::ExprPtr build_run(NodeState& state, const std::vector<const ChildItem*>& run, jsx::Span& span) {
    span = run_span(run);
    std::vector<ir::TemplatePart> parts;
    std::string text;
    bool has_expression = false;
    for (const ChildItem* item : run) {
        if (item->kind == ChildItem::Kind::Text) {
            text += item->node->text;
            continue;
        }
        ir::ExprPtr lowered = lower_node_expression(state, *item->expr);
        if (!lowered) {
            return nullptr;
        }
        if (!text.empty()) {
            parts.push_back(ir::text_part(std::move(text)));
            text.clear();
        }
        parts.push_back(ir::expr_part(std::move(lowered)));
        has_expression = true;
    }
    if (!text.empty()) {
        parts.push_back(ir::text_part(std::move(text)));
    }
    if (parts.empty()) {
        return ir::make_literal(std::string());
    }
    if (!has_expression) {
        std::string merged;
        for (const ir::TemplatePart& part : parts) {
            merged += part.text;
        }
        return ir::make_literal(std::move(merged));
    }
    return ir::make_template(std::move(parts));
}

bool flush_run(NodeState& state, const std::vector<const ChildItem*>& run, ir::Node& parent) {
    if (run.empty()) {
        return true;
    }
    jsx::Span span;
    ir::ExprPtr text = build_run(state, run, span);
    if (!text) {
        return false;
    }
    ir::Node text_node;
    text_node.kind = ir::NodeKind::Text;
    text_node.id = next_node_id(state);
    text_node.source = to_source_span(state.context.source_map, span, state.context.file);
    text_node.text = std::move(text);
    parent.children.push_back(std::move(text_node));
    return true;
}

// Appends one element, conditional or loop item to `parent`; false on error.
bool append_node_item(NodeState& state, const ChildItem& item, ir::Node& parent) {
    if (item.kind == ChildItem::Kind::Element) {
        std::optional<ir::Node> child = make_node(state, *item.node);
        if (!child) {
            return false;
        }
        parent.children.push_back(std::move(*child));
        return true;
    }
    std::vector<ir::Node> produced;
    if (!lower_control_child(state, *item.expr, produced)) {
        return false;
    }
    for (ir::Node& node : produced) {
        parent.children.push_back(std::move(node));
    }
    return true;
}

// View and Button with element or control children: every run becomes one synthetic Text child in
// source order (decision 11); element and control children are lowered in place.
bool append_runs_and_elements(NodeState& state, const std::vector<ChildItem>& items,
                              ir::Node& parent) {
    std::vector<const ChildItem*> run;
    for (const ChildItem& item : items) {
        if (is_run_item(item)) {
            run.push_back(&item);
            continue;
        }
        if (!flush_run(state, run, parent)) {
            return false;
        }
        run.clear();
        if (!append_node_item(state, item, parent)) {
            return false;
        }
    }
    return flush_run(state, run, parent);
}

bool lower_text_children(NodeState& state, const std::vector<ChildItem>& items, ir::Node& node) {
    for (const ChildItem& item : items) {
        if (!is_run_item(item)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<Text>` accepts one text run; an element, conditional or loop child is "
                      "not supported (subset 10.4)",
                      item.node->span);
            return false;
        }
    }
    std::vector<const ChildItem*> run;
    run.reserve(items.size());
    for (const ChildItem& item : items) {
        run.push_back(&item);
    }
    jsx::Span span;
    ir::ExprPtr text = build_run(state, run, span);
    if (!text) {
        return false;
    }
    node.text = std::move(text);
    return true;
}

bool lower_button_children(NodeState& state, const std::vector<ChildItem>& items, ir::Node& node) {
    const bool has_element = std::any_of(items.begin(), items.end(),
                                         [](const ChildItem& item) { return !is_run_item(item); });
    if (has_element) {
        return append_runs_and_elements(state, items, node);
    }
    // Text-only children are the label (decision 11); an empty Button has neither, which the
    // analysis/schema constraints report.
    if (items.empty()) {
        return true;
    }
    std::vector<const ChildItem*> run;
    run.reserve(items.size());
    for (const ChildItem& item : items) {
        run.push_back(&item);
    }
    jsx::Span span;
    ir::ExprPtr label = build_run(state, run, span);
    if (!label) {
        return false;
    }
    node.props[std::string("label")] = std::move(label);
    return true;
}

} // namespace

// Slice 9: the children of a component reference are the caller's static slot content. They follow
// the View rules (text runs become synthetic Text children) and are validated against the target
// component's `children` prop by slice 4, not here.
bool lower_slot_children(NodeState& state, const jsx::JsxNode& element, ir::Node& node) {
    std::vector<ChildItem> items;
    if (!flatten_children(state, element, items, false)) {
        return false;
    }
    return append_runs_and_elements(state, items, node);
}

bool lower_children(NodeState& state, const jsx::JsxNode& element, const ElementInfo& info,
                    ir::Node& node) {
    const schema::ChildrenKind kind = info.row->children;
    if (kind == schema::ChildrenKind::Template) {
        // The virtualized item template needs the `itemTemplate` IR field that ir-v1 does not have
        // yet (Q9 of jsx-intrinsics-v1).
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`<" + std::string(element.tag) +
                      ">` needs the `itemTemplate` IR field, which ir-v1 does not have yet (Q9)",
                  element.span);
        return false;
    }
    // ErrorBoundary requires one element child and rejects fragments (elements-other.md 4).
    const bool reject_fragments = kind == schema::ChildrenKind::One;
    std::vector<ChildItem> items;
    if (!flatten_children(state, element, items, reject_fragments)) {
        return false;
    }
    switch (kind) {
    case schema::ChildrenKind::None:
        if (!items.empty()) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<" + std::string(element.tag) + ">` accepts no children",
                      items.front().node->span);
            return false;
        }
        return true;
    case schema::ChildrenKind::Text:
        return lower_text_children(state, items, node);
    case schema::ChildrenKind::TextAndNodes:
        if (element.tag == "Button") {
            return lower_button_children(state, items, node);
        }
        return append_runs_and_elements(state, items, node);
    case schema::ChildrenKind::Nodes: {
        for (const ChildItem& item : items) {
            if (is_run_item(item)) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "`<" + std::string(element.tag) +
                              ">` accepts element children only; wrap text in `<Text>`",
                          item.node->span);
                return false;
            }
            if (!append_node_item(state, item, node)) {
                return false;
            }
        }
        return true;
    }
    case schema::ChildrenKind::One: {
        for (const ChildItem& item : items) {
            if (item.kind == ChildItem::Kind::Conditional || item.kind == ChildItem::Kind::Loop) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "`<" + std::string(element.tag) +
                              ">` does not accept conditional or loop children "
                              "(elements-other.md section 4)",
                          item.node->span);
                return false;
            }
        }
        if (items.size() != 1 || items.front().kind != ChildItem::Kind::Element) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<" + std::string(element.tag) + ">` needs exactly one element child",
                      element.span);
            return false;
        }
        std::optional<ir::Node> child = make_node(state, *items.front().node);
        if (!child) {
            return false;
        }
        node.children.push_back(std::move(*child));
        return true;
    }
    case schema::ChildrenKind::Template:
        break;
    }
    return false;
}

} // namespace tessera::compiler::lowering::internal
