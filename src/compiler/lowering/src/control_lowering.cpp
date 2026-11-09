// SPDX-License-Identifier: MIT

// Slice 8: conditional children and `.map` list loops (docs/specs/jsx-subset-v1.md sections 10.4
// and 10.5, docs/specs/ir-v1.md sections 3 and 5, docs/design/jsx-ir-lowering.md section 2).
//
// A conditional child contributes one or two sibling nodes to the parent with complementary
// `condition` expressions; a `.map` child contributes the template node with a `loop` and the
// `key` expression of the element. Node ids are assigned in source order for every branch at
// lowering time, whether or not the branch is taken at run time (deterministic ids).

#include "node_lowering_internal.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

bool is_jsx(const jsx::Expr& expr) {
    return expr.kind == jsx::ExprKind::Jsx && expr.jsx != nullptr;
}

bool is_element(const jsx::Expr& expr) {
    return is_jsx(expr) && expr.jsx->kind == jsx::JsxKind::Element;
}

bool is_fragment(const jsx::Expr& expr) {
    return is_jsx(expr) && expr.jsx->kind == jsx::JsxKind::Fragment;
}

// `null` and the literal `false` render nothing (subset 10.4 and elements-basic.md section 0).
bool is_nothing(const jsx::Expr& expr) {
    if (expr.kind == jsx::ExprKind::Null) {
        return true;
    }
    return expr.kind == jsx::ExprKind::Boolean && !expr.boolean;
}

ir::ExprPtr negate(ir::ExprPtr value) {
    return ir::make_unary("!", std::move(value));
}

bool append_branch(NodeState& state, const jsx::Expr& branch, ir::ExprPtr condition,
                   std::vector<ir::Node>& nodes) {
    std::optional<ir::Node> node = make_node(state, *branch.jsx);
    if (!node) {
        return false;
    }
    node->condition = std::move(condition);
    nodes.push_back(std::move(*node));
    return true;
}

bool lower_and_conditional(NodeState& state, const jsx::Expr& expr, std::vector<ir::Node>& nodes) {
    if (expr.text != "&&") {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "conditional rendering supports `cond && <X/>` and `cond ? <X/> : <Y/>` only",
                  expr.span);
        return false;
    }
    if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
        fail_node(state, "TESSERA_IR_MALFORMED", "`&&` needs two operands", expr.span);
        return false;
    }
    const jsx::Expr& guarded = *expr.operands[1];
    if (!is_element(guarded)) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  is_fragment(guarded)
                      ? "a conditional branch must be one element, not a fragment (slice 8)"
                      : "the right side of `&&` must be one element",
                  guarded.span);
        return false;
    }
    ir::ExprPtr condition = lower_node_expression(state, *expr.operands[0]);
    if (!condition) {
        return false;
    }
    return append_branch(state, guarded, std::move(condition), nodes);
}

bool lower_ternary(NodeState& state, const jsx::Expr& expr, std::vector<ir::Node>& nodes) {
    if (expr.operands.size() != 3 || !expr.operands[0] || !expr.operands[1] || !expr.operands[2]) {
        fail_node(state, "TESSERA_IR_MALFORMED", "a conditional needs three operands", expr.span);
        return false;
    }
    const jsx::Expr& test = *expr.operands[0];
    const jsx::Expr& then_branch = *expr.operands[1];
    const jsx::Expr& else_branch = *expr.operands[2];
    const bool then_element = is_element(then_branch);
    const bool else_element = is_element(else_branch);
    if (!then_element && !else_element) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a conditional child needs an element branch (slice 8)", expr.span);
        return false;
    }
    if ((is_jsx(then_branch) && !then_element) || (is_jsx(else_branch) && !else_element)) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a conditional branch must be one element, not a fragment (slice 8)", expr.span);
        return false;
    }
    if (then_element && else_element) {
        ir::ExprPtr then_condition = lower_node_expression(state, test);
        if (!then_condition) {
            return false;
        }
        if (!append_branch(state, then_branch, std::move(then_condition), nodes)) {
            return false;
        }
        ir::ExprPtr else_condition = lower_node_expression(state, test);
        if (!else_condition) {
            return false;
        }
        return append_branch(state, else_branch, negate(std::move(else_condition)), nodes);
    }
    if (then_element) {
        if (!is_nothing(else_branch)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "the other branch of a conditional child must be `null` or `false`",
                      else_branch.span);
            return false;
        }
        ir::ExprPtr condition = lower_node_expression(state, test);
        return condition && append_branch(state, then_branch, std::move(condition), nodes);
    }
    if (!is_nothing(then_branch)) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "the other branch of a conditional child must be `null` or `false`",
                  then_branch.span);
        return false;
    }
    ir::ExprPtr condition = lower_node_expression(state, test);
    return condition && append_branch(state, else_branch, negate(std::move(condition)), nodes);
}

bool lower_loop(NodeState& state, const jsx::Expr& expr, std::vector<ir::Node>& nodes) {
    if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a list loop is written `list.map(callback)` with one callback", expr.span);
        return false;
    }
    const jsx::Expr& callee = *expr.operands[0];
    if (callee.kind != jsx::ExprKind::Member || callee.operands.size() != 1 ||
        !callee.operands[0]) {
        fail_node(state, "TESSERA_IR_MALFORMED", "a loop callee is a member call", expr.span);
        return false;
    }
    const jsx::Expr& callback = *expr.operands[1];
    if (callback.kind != jsx::ExprKind::Arrow) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`.map` takes an inline arrow callback (subset 10.5)", callback.span);
        return false;
    }
    if (callback.arrow_block_body) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "the `.map` callback body must be one element, not a block (subset 10.5)",
                  callback.span);
        return false;
    }
    if (callback.params.empty() || callback.params.size() > 2) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "the `.map` callback takes the item and an optional index (subset 10.5)",
                  callback.span);
        return false;
    }
    if (callback.operands.size() != 1 || !callback.operands[0]) {
        fail_node(state, "TESSERA_IR_MALFORMED", "the `.map` callback has no body", callback.span);
        return false;
    }
    const jsx::Expr& body = *callback.operands[0];
    if (!is_element(body)) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  is_fragment(body)
                      ? "a `.map` callback must return one element, not a fragment (slice 8)"
                      : "a `.map` callback must return one element",
                  body.span);
        return false;
    }
    if (state.loop_depth >= 2) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "nested list loops deeper than 2 are not supported (ir-v1 section 5)", expr.span);
        return false;
    }
    // The source is the receiver of `.map`; it must be a list name (a ref), not an arbitrary
    // expression (subset 10.5).
    ir::ExprPtr source = lower_node_expression(state, *callee.operands[0]);
    if (!source) {
        return false;
    }
    if (source->kind != ir::ExprKind::Ref) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a list loop source must be a list name or a constant list (subset 10.5)",
                  callee.operands[0]->span);
        return false;
    }
    // The template sees the item (and index) as locals; they shadow outer names (decision 8).
    const std::size_t mark = state.names.local_count();
    state.names.add_local(callback.params[0]);
    if (callback.params.size() == 2) {
        state.names.add_local(callback.params[1]);
    }
    ++state.loop_depth;
    std::optional<ir::Node> template_node = make_node(state, *body.jsx);
    --state.loop_depth;
    state.names.truncate_locals(mark);
    if (!template_node) {
        return false;
    }
    if (!template_node->key) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "the root element of a `.map` callback needs `key={...}` (subset 10.5)",
                  body.jsx->span);
        return false;
    }
    ir::Loop loop;
    loop.item = callback.params[0];
    loop.index = callback.params.size() == 2 ? callback.params[1] : "";
    loop.source = std::move(source);
    template_node->loop = std::move(loop);
    nodes.push_back(std::move(*template_node));
    return true;
}

} // namespace

ControlKind classify_control(const jsx::Expr& expr) {
    if (expr.kind == jsx::ExprKind::Conditional) {
        if (expr.operands.size() != 3) {
            return ControlKind::Conditional; // reported as malformed by the lowering
        }
        const bool has_jsx = (expr.operands[1] && is_jsx(*expr.operands[1])) ||
                             (expr.operands[2] && is_jsx(*expr.operands[2]));
        return has_jsx ? ControlKind::Conditional : ControlKind::None;
    }
    if (expr.kind == jsx::ExprKind::Binary && (expr.text == "&&" || expr.text == "||") &&
        expr.operands.size() == 2 && expr.operands[1] && is_jsx(*expr.operands[1])) {
        return ControlKind::Conditional;
    }
    if (expr.kind == jsx::ExprKind::Call && !expr.operands.empty() && expr.operands[0] &&
        expr.operands[0]->kind == jsx::ExprKind::Member && expr.operands[0]->text == "map") {
        return ControlKind::Loop;
    }
    return ControlKind::None;
}

bool lower_control_child(NodeState& state, const jsx::Expr& expr, std::vector<ir::Node>& nodes) {
    if (expr.kind == jsx::ExprKind::Conditional) {
        return lower_ternary(state, expr, nodes);
    }
    if (expr.kind == jsx::ExprKind::Binary) {
        return lower_and_conditional(state, expr, nodes);
    }
    return lower_loop(state, expr, nodes);
}

} // namespace tessera::compiler::lowering::internal
