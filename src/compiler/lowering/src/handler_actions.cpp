// SPDX-License-Identifier: MIT

// Inline setter handlers (docs/specs/jsx-subset-v1.md section 9.2, docs/specs/ir-v1.md section
// 2.1): the statements of a setter-call arrow become ordered IR actions.

#include "hook_lowering_internal.hpp"

#include <cmath>
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

// The `spring` option: `true` (all defaults) or an object literal of optional number-literal
// `stiffness`, `damping` and `mass`. Ranges are the IR validator's job.
bool spring_option(const jsx::Expr& value, ir::SpringTiming& spring) {
    if (value.kind == jsx::ExprKind::Boolean) {
        return value.boolean;
    }
    if (value.kind != jsx::ExprKind::Object) {
        return false;
    }
    bool seen_stiffness = false;
    bool seen_damping = false;
    bool seen_mass = false;
    for (const jsx::ObjectProperty& property : value.properties) {
        const jsx::Expr* number = property.value.get();
        if (number == nullptr || number->kind != jsx::ExprKind::Number ||
            !std::isfinite(number->number)) {
            return false;
        }
        bool* seen = nullptr;
        double* field = nullptr;
        if (property.key == "stiffness") {
            seen = &seen_stiffness;
            field = &spring.stiffness;
        } else if (property.key == "damping") {
            seen = &seen_damping;
            field = &spring.damping;
        } else if (property.key == "mass") {
            seen = &seen_mass;
            field = &spring.mass;
        } else {
            return false;
        }
        if (*seen) {
            return false;
        }
        *seen = true;
        *field = number->number;
    }
    return true;
}

// The options of an animated-value setter (jsx-subset-v1.md section 7): `animate` (boolean),
// `duration` (a non-negative number literal, milliseconds), `easing` (a string literal) and
// `spring` (`true` or an object of number literals). They
// are literals so the IR stays static; anything else is rejected rather than ignored. Whether the
// easing text is valid and whether the combination makes sense is the IR validator's job.
bool animated_options(HookState& state, const jsx::Expr& options, ir::Action& action) {
    constexpr const char* kShape =
        "the options of an animated-value setter are `{ animate, duration, easing, spring }` with "
        "a boolean, a non-negative number literal (ms), a string literal and `true` or an "
        "object literal of `stiffness`, `damping` and `mass` number literals";
    if (options.kind != jsx::ExprKind::Object) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED", kShape, options.span);
        return false;
    }
    bool seen_animate = false;
    bool seen_duration = false;
    bool seen_easing = false;
    bool seen_spring = false;
    for (const jsx::ObjectProperty& property : options.properties) {
        const jsx::Expr* value = property.value.get();
        if (value == nullptr) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", kShape, options.span);
            return false;
        }
        const bool is_animate = property.key == "animate" && !seen_animate;
        const bool is_duration = property.key == "duration" && !seen_duration;
        const bool is_easing = property.key == "easing" && !seen_easing;
        const bool is_spring = property.key == "spring" && !seen_spring;
        if (is_animate && value->kind == jsx::ExprKind::Boolean) {
            seen_animate = true;
            action.animate = value->boolean;
        } else if (is_duration && value->kind == jsx::ExprKind::Number &&
                   std::isfinite(value->number) && value->number >= 0.0) {
            seen_duration = true;
            action.duration_ms = value->number;
        } else if (is_easing && value->kind == jsx::ExprKind::String) {
            seen_easing = true;
            action.easing = value->text;
        } else if (is_spring && spring_option(*value, action.spring.emplace())) {
            seen_spring = true;
        } else {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", kShape, value->span);
            return false;
        }
    }
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
    // `v.set(value)` or `v.set(value, { animate, duration, easing, spring })`.
    if (call.operands.size() < 2 || call.operands.size() > 3 || !callee.operands[0]) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "an animated-value setter takes a value and optional `{ animate, duration, "
                  "easing, spring }`",
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
    ir::Action action;
    action.kind = ir::ActionKind::AnimSet;
    if (call.operands.size() == 3 && !animated_options(state, *call.operands[2], action)) {
        return std::nullopt;
    }
    ir::ExprPtr value = lower_component_expression(state, *call.operands[1], names);
    if (!value) {
        return std::nullopt;
    }
    action.target = "anim::" + animated->name;
    action.value = std::move(value);
    return action;
}

// The first `count` statements of `arrow` as actions.
std::vector<ir::Action> lower_statements(HookState& state, const jsx::Expr& arrow,
                                         std::size_t count) {
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
    for (std::size_t i = 0; i < count; ++i) {
        const jsx::ExprPtr& statement = arrow.operands[i];
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

} // namespace

std::vector<ir::Action> lower_actions(HookState& state, const jsx::Expr& arrow) {
    if (arrow.kind != jsx::ExprKind::Arrow) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "only an arrow function has inline handler actions", arrow.span);
        return {};
    }
    return lower_statements(state, arrow, arrow.operands.size());
}

std::optional<ir::Preview> lower_preview(HookState& state, const jsx::Expr& arrow) {
    if (arrow.kind != jsx::ExprKind::Arrow || arrow.operands.empty()) {
        return std::nullopt;
    }
    // A scratch state: a body outside the preview rules is not an error here.
    HookState scratch{state.context, std::nullopt};
    std::size_t count = arrow.operands.size();
    ir::ExprPtr return_value;
    const jsx::ExprPtr& last = arrow.operands.back();
    if (last && last->kind == jsx::ExprKind::Return) {
        --count;
        if (last->operands.size() == 1 && last->operands[0]) {
            ComponentNames names(scratch.context.analysis);
            for (const std::string& param : arrow.params) {
                names.add_local(param);
            }
            return_value = lower_component_expression(scratch, *last->operands[0], names);
            if (scratch.error || !return_value) {
                return std::nullopt;
            }
        }
    }
    std::vector<ir::Action> actions = lower_statements(scratch, arrow, count);
    if (scratch.error) {
        return std::nullopt;
    }
    return ir::Preview{std::move(actions), std::move(return_value)};
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
