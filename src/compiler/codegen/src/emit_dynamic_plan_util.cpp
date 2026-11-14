// SPDX-License-Identifier: MIT

// Helpers of the dynamic-plan walk, split out of emit_dynamic_plan.cpp (docs/design/compiler.md
// 11).

#include "emit_dynamic_plan_util.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace tessera::compiler::codegen::internal::plan_detail {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool reserved_loop_local(std::string_view name) {
    static constexpr std::array<std::string_view, 11> kReserved = {
        "format_value",  "apply_state", "attach_child_at", "detach_child", "kEffectHashSeed",
        "kItemIdStride", "build_item",  "apply_item",      "apply_list",   "apply_children"};
    if (starts_with(name, "tessera_") || starts_with(name, "node_")) {
        return true;
    }
    return std::find(kReserved.begin(), kReserved.end(), name) != kReserved.end();
}

std::optional<std::string> declared_field_type(const ir::Component& component,
                                               std::string_view ref_id) {
    for (const ir::State& state : component.state) {
        if (state.id == ref_id) {
            return state.type;
        }
    }
    if (starts_with(ref_id, "prop::")) {
        const std::string_view name = ref_id.substr(6);
        for (const ir::Prop& prop : component.props) {
            if (prop.name == name) {
                return prop.type;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> list_element_type(std::string_view type) {
    if (!starts_with(type, "list<") || type.back() != '>') {
        return std::nullopt;
    }
    return std::string(type.substr(5, type.size() - 6));
}

bool mentions_loop_local(const ir::Expr& expr, std::string_view item, std::string_view index) {
    switch (expr.kind) {
    case ir::ExprKind::Ref: {
        const std::string_view base = std::string_view(expr.id).substr(0, expr.id.find('.'));
        return base == item || (!index.empty() && base == index);
    }
    case ir::ExprKind::Literal:
        return false;
    case ir::ExprKind::Template:
        for (const ir::TemplatePart& part : expr.parts) {
            if (part.expr && mentions_loop_local(*part.expr, item, index)) {
                return true;
            }
        }
        return false;
    default:
        for (const ir::ExprPtr& operand : expr.operands) {
            if (operand && mentions_loop_local(*operand, item, index)) {
                return true;
            }
        }
        return false;
    }
}

} // namespace tessera::compiler::codegen::internal::plan_detail
