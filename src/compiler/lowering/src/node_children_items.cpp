// SPDX-License-Identifier: MIT

// Child flattening and run construction of node lowering (slice 7): the JSX children of one element
// become ChildItem values; runs become synthetic Text children (elements-basic.md section 0). Moved
// out of node_children.cpp when inline spans (issue #229) pushed that file over the size limit.

#include "node_lowering_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::lowering::internal {

bool is_run_item(const ChildItem& item) {
    return item.kind == ChildItem::Kind::Text || item.kind == ChildItem::Kind::Expression;
}

// Section 3.4: `{children}` of a component that declares the slot is the Slot position, not an
// ordinary expression child. The name resolver maps the destructured `children` prop to
// `prop::children`; a local of the same name resolves to itself and is unaffected.
bool is_children_slot(const NodeState& state, const jsx::Expr& expr) {
    if (expr.kind != jsx::ExprKind::Identifier) {
        return false;
    }
    const std::optional<std::string> id = state.names.resolve(expr.text);
    return id.has_value() && *id == "prop::children";
}

// `{props.children}` of a non-destructured props parameter: the spec accepts only the destructured
// `{children}` form (jsx-subset-v1.md section 5), so this gets a diagnostic with the fix.
bool is_props_children_member(const NodeState& state, const jsx::Expr& expr) {
    if (expr.kind != jsx::ExprKind::Member || expr.text != "children" ||
        expr.operands.size() != 1 || !expr.operands[0] ||
        expr.operands[0]->kind != jsx::ExprKind::Identifier) {
        return false;
    }
    const std::optional<std::string> id =
        state.names.resolve(expr.operands[0]->text + "." + expr.text);
    return id.has_value() && *id == "prop::children";
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
            if (is_children_slot(state, expr)) {
                items.push_back(ChildItem{ChildItem::Kind::Slot, child.get(), nullptr});
                break;
            }
            if (is_props_children_member(state, expr)) {
                fail_node(state, "TESSERA_IR_SLOT",
                          "a non-destructured props parameter cannot place the children slot",
                          expr.span, "destructure the prop: `{ children }: CardProps`");
                return false;
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

// Appends one element, conditional, loop or slot item to `parent`; false on error.
bool append_node_item(NodeState& state, const ChildItem& item, ir::Node& parent) {
    if (item.kind == ChildItem::Kind::Slot) {
        // Section 3.4: the callee's `{children}` position becomes a bare Slot node.
        if (state.loop_depth > 0) {
            fail_node(state, "TESSERA_IR_SLOT", "a Slot is not inside a loop template",
                      item.node->span);
            return false;
        }
        if (++state.slot_count > 1) {
            fail_node(state, "TESSERA_IR_SLOT", "a component has at most one Slot",
                      item.node->span);
            return false;
        }
        ir::Node slot;
        slot.kind = ir::NodeKind::Slot;
        slot.id = next_node_id(state);
        slot.source = to_source_span(state.context.source_map, item.node->span, state.context.file);
        parent.children.push_back(std::move(slot));
        return true;
    }
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

} // namespace tessera::compiler::lowering::internal
