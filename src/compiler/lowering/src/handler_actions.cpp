// SPDX-License-Identifier: MIT

// Inline setter handlers (docs/specs/jsx-subset-v1.md section 9.2, docs/specs/ir-v1.md section
// 2.1): the statements of a setter-call arrow become ordered IR actions.

#include "hook_lowering_internal.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kBodyRule =
    "an inline handler body is a sequence of setter calls and animated-value `.set(...)` calls "
    "(jsx-subset-v1.md section 9.2)";

// The `x` of the `useState` statement a setter belongs to.
const rules::Symbol* state_for_setter(const rules::ComponentAnalysis& analysis,
                                      const rules::Symbol& setter) {
    for (const rules::Symbol& symbol : analysis.symbols) {
        if (symbol.kind == rules::SymbolKind::State && symbol.statement == setter.statement) {
            return &symbol;
        }
    }
    return nullptr;
}

bool animate_flag(HookState& state, const jsx::Expr& options, bool& animate) {
    // The only option of an animated-value setter is `{ animate: false }` (jsx-subset-v1.md
    // section 7). Anything else is rejected rather than ignored.
    if (options.kind != jsx::ExprKind::Object || options.properties.size() != 1 ||
        options.properties[0].key != "animate" || !options.properties[0].value ||
        options.properties[0].value->kind != jsx::ExprKind::Boolean) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "the options of an animated-value setter must be `{ animate: false }`",
                  options.span);
        return false;
    }
    animate = options.properties[0].value->boolean;
    return true;
}

std::optional<ir::Action> lower_setter_call(HookState& state, const jsx::Expr& call,
                                            const jsx::Expr& callee, const ComponentNames& names) {
    const rules::Symbol* setter = state.context.analysis.find(callee.text);
    const rules::Symbol* target =
        setter != nullptr ? state_for_setter(state.context.analysis, *setter) : nullptr;
    if (setter == nullptr || setter->kind != rules::SymbolKind::StateSetter || target == nullptr) {
        return std::nullopt; // not a setter reference: the caller reports the body rule
    }
    if (call.operands.size() != 2) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "setter `" + callee.text + "` takes exactly one value", call.span, kBodyRule);
        return std::nullopt;
    }
    ir::ExprPtr value = lower_component_expression(state, *call.operands[1], names);
    if (!value) {
        return std::nullopt;
    }
    ir::Action action;
    action.kind = ir::ActionKind::Set;
    action.target = "state::" + target->name;
    action.value = std::move(value);
    return action;
}

std::optional<ir::Action> lower_animated_set(HookState& state, const jsx::Expr& call,
                                             const jsx::Expr& callee, const ComponentNames& names) {
    // `v.set(value)` or `v.set(value, { animate: false })`.
    if (call.operands.size() < 2 || call.operands.size() > 3 || !callee.operands[0]) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "an animated-value setter takes a value and optional `{ animate: false }`",
                  call.span, kBodyRule);
        return std::nullopt;
    }
    const jsx::Expr& receiver = *callee.operands[0];
    const rules::Symbol* animated = receiver.kind == jsx::ExprKind::Identifier
                                        ? state.context.analysis.find(receiver.text)
                                        : nullptr;
    if (animated == nullptr || animated->kind != rules::SymbolKind::AnimatedValue) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED", kBodyRule, call.span);
        return std::nullopt;
    }
    bool animate = true;
    if (call.operands.size() == 3 && !animate_flag(state, *call.operands[2], animate)) {
        return std::nullopt;
    }
    ir::ExprPtr value = lower_component_expression(state, *call.operands[1], names);
    if (!value) {
        return std::nullopt;
    }
    ir::Action action;
    action.kind = ir::ActionKind::AnimSet;
    action.target = "anim::" + animated->name;
    action.value = std::move(value);
    action.animate = animate;
    return action;
}

} // namespace

std::vector<ir::Action> lower_actions(HookState& state, const jsx::Expr& arrow) {
    std::vector<ir::Action> actions;
    if (arrow.kind != jsx::ExprKind::Arrow) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "only an arrow function has inline handler actions", arrow.span);
        return actions;
    }
    ComponentNames names(state.context.analysis);
    for (const std::string& param : arrow.params) {
        names.add_local(param);
    }
    for (const jsx::ExprPtr& statement : arrow.operands) {
        if (!statement || statement->kind != jsx::ExprKind::Call || statement->operands.empty() ||
            !statement->operands[0]) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", kBodyRule,
                      statement ? statement->span : arrow.span);
            return actions;
        }
        const jsx::Expr& callee = *statement->operands[0];
        std::optional<ir::Action> action;
        if (callee.kind == jsx::ExprKind::Member && callee.text == "set" &&
            callee.operands.size() == 1 && callee.operands[0] &&
            callee.operands[0]->kind == jsx::ExprKind::Identifier) {
            action = lower_animated_set(state, *statement, callee, names);
        } else if (callee.kind == jsx::ExprKind::Identifier) {
            action = lower_setter_call(state, *statement, callee, names);
        }
        if (state.error) {
            return actions;
        }
        if (!action) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", kBodyRule, statement->span);
            return actions;
        }
        actions.push_back(std::move(*action));
    }
    return actions;
}

std::vector<ir::Param> lower_handler_params(HookState& state, const jsx::Expr& arrow) {
    std::vector<ir::Param> params;
    if (arrow.params.size() != arrow.param_types.size()) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "a handler arrow has one parameter type per parameter", arrow.span);
        return params;
    }
    for (std::size_t i = 0; i < arrow.params.size(); ++i) {
        const std::string& name = arrow.params[i];
        if (name.empty()) {
            fail_hook(state, "TESSERA_IR_MALFORMED", "a handler parameter has no name", arrow.span);
            return params;
        }
        if (!arrow.param_types[i]) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                      "parameter `" + name +
                          "` of a declared handler needs a type annotation; the event parameter "
                          "types of the element table are slice 4",
                      arrow.span, "annotate the parameter type");
            return params;
        }
        const std::optional<std::string> type = lower_written_type(*arrow.param_types[i]);
        if (!type) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                      "parameter `" + name + "` has a type with no IR form in v1",
                      arrow.param_types[i]->span);
            return params;
        }
        params.push_back(ir::Param{name, *type});
    }
    return params;
}

} // namespace tessera::compiler::lowering::internal

namespace tessera::compiler::lowering {

HandlerActionsResult lower_handler_actions(const jsx::Expr& arrow,
                                           const HookLoweringContext& context) {
    internal::HookState state{context, std::nullopt};
    std::vector<ir::Action> actions = internal::lower_actions(state, arrow);
    if (state.error) {
        return std::unexpected(HookLoweringErrors{std::move(*state.error)});
    }
    return actions;
}

} // namespace tessera::compiler::lowering
