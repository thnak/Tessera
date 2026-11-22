// SPDX-License-Identifier: MIT

// ADR-066 item 3: whether a component needs `tessera/ui/scene/list_ops.hpp`. The include is emitted
// only when one does, so a component that uses no list or string method keeps the exact bytes it
// had before the helpers existed. Split out of `emit_header.cpp` when that file reached the
// 500-line hard limit; the subject is the walk, not the header.

#include "codegen_internal.hpp"

#include <string_view>

namespace tessera::compiler::codegen::internal {
namespace {

// The pure calls that `list_ops.hpp` defines. `slice` is both a list and a string form, so it is
// listed once.
constexpr std::string_view kListOps[] = {"trim",       "toUpperCase", "toLowerCase",
                                         "startsWith", "slice",       "toFixed"};

bool uses_list_ops(const ir::Expr& expr);

bool uses_list_ops_in(const ir::Expr* expr) {
    return expr != nullptr && uses_list_ops(*expr);
}

bool uses_list_ops(const ir::Expr& expr) {
    if (expr.kind == ir::ExprKind::ListFilter || expr.kind == ir::ExprKind::ListSlice) {
        return true;
    }
    if (expr.kind == ir::ExprKind::Call) {
        for (const std::string_view name : kListOps) {
            if (expr.id == name) {
                return true;
            }
        }
    }
    for (const ir::ExprPtr& operand : expr.operands) {
        if (uses_list_ops_in(operand.get())) {
            return true;
        }
    }
    for (const ir::RecordField& field : expr.fields) {
        if (uses_list_ops_in(field.value.get())) {
            return true;
        }
    }
    for (const ir::TemplatePart& part : expr.parts) {
        if (uses_list_ops_in(part.expr.get())) {
            return true;
        }
    }
    return false;
}

bool uses_list_ops(const ir::Node& node) {
    if (uses_list_ops_in(node.text.get()) || uses_list_ops_in(node.key.get()) ||
        uses_list_ops_in(node.condition.get())) {
        return true;
    }
    for (const auto& [name, value] : node.props) {
        if (uses_list_ops_in(value.get())) {
            return true;
        }
    }
    for (const auto& [name, value] : node.style) {
        if (uses_list_ops_in(value.get())) {
            return true;
        }
    }
    if (node.loop && uses_list_ops_in(node.loop->source.get())) {
        return true;
    }
    for (const ir::Node& child : node.children) {
        if (uses_list_ops(child)) {
            return true;
        }
    }
    return node.fallback != nullptr && uses_list_ops(*node.fallback);
}

} // namespace

bool uses_list_ops(const ir::Component& component) {
    for (const ir::State& state : component.state) {
        if (uses_list_ops_in(state.initial.get())) {
            return true;
        }
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (uses_list_ops_in(value.initial.get())) {
            return true;
        }
    }
    for (const ir::Handler& handler : component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (uses_list_ops_in(action.value.get()) || uses_list_ops_in(action.guard.get())) {
                return true;
            }
        }
    }
    return uses_list_ops(component.root);
}

} // namespace tessera::compiler::codegen::internal
