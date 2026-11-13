// SPDX-License-Identifier: MIT

// Inline setter handlers (docs/specs/jsx-subset-v1.md section 9.2, docs/specs/ir-v1.md section
// 2.1): the statements of a setter-call arrow become ordered IR actions.

#include "hook_lowering_internal.hpp"

#include <algorithm>
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

// The `handler::N` of the parameterless inline handler `name` declared in this component, or
// nullopt for anything else (undeclared, native `bind::`, with parameters, not an arrow). The id
// is derived from the declaration order exactly as `lower_hooks` numbers inline handlers, so a
// handler declared after the setter call resolves without lowering it first, and a handler that
// completes into itself needs no recursion.
std::optional<std::string> declared_inline_handler_id(const HookState& state,
                                                      const std::string& name) {
    unsigned number = 0;
    for (const jsx::Statement& statement : state.context.component.body) {
        if (!statement.init || statement.init->kind != jsx::ExprKind::Arrow ||
            statement.names.empty() || !statement.bind_id.empty()) {
            continue;
        }
        const rules::ArrowInfo* info = state.context.analysis.find_arrow(statement.init.get());
        if (info == nullptr || info->role != rules::ArrowRole::InlineHandler) {
            continue;
        }
        ++number;
        const bool named = std::find(statement.names.begin(), statement.names.end(), name) !=
                           statement.names.end();
        if (named) {
            if (!statement.init->params.empty()) {
                return std::nullopt;
            }
            return "handler::" + std::to_string(number);
        }
    }
    return std::nullopt;
}

// The options of an animated-value setter (jsx-subset-v1.md section 7): `animate` (boolean),
// `duration` (a non-negative number literal, milliseconds), `easing` (a string literal),
// `spring` (`true` or an object of number literals) and `onComplete` (the name of a parameterless
// inline handler of the component). They are literals or names so the IR stays static; anything
// else is rejected rather than ignored. Whether the easing text is valid and whether the
// combination makes sense is the IR validator's job.
bool animated_options(HookState& state, const jsx::Expr& options, ir::Action& action) {
    constexpr const char* kShape =
        "the options of an animated-value setter are `{ animate, duration, easing, spring, "
        "onComplete }` with a boolean, a non-negative number literal (ms), a string literal, "
        "`true` or an object literal of `stiffness`, `damping` and `mass` number literals, and "
        "the name of a parameterless inline handler declared in this component";
    if (options.kind != jsx::ExprKind::Object) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED", kShape, options.span);
        return false;
    }
    bool seen_animate = false;
    bool seen_duration = false;
    bool seen_easing = false;
    bool seen_spring = false;
    bool seen_on_complete = false;
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
        const bool is_on_complete = property.key == "onComplete" && !seen_on_complete;
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
        } else if (is_on_complete && value->kind == jsx::ExprKind::Identifier) {
            std::optional<std::string> id = declared_inline_handler_id(state, value->text);
            if (!id) {
                fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                          "onComplete `" + value->text +
                              "` is not a parameterless inline handler declared in this component",
                          value->span, kShape);
                return false;
            }
            seen_on_complete = true;
            action.on_complete = std::move(*id);
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
    // `v.set(value)` or `v.set(value, { animate, duration, easing, spring, onComplete })`.
    if (call.operands.size() < 2 || call.operands.size() > 3 || !callee.operands[0]) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "an animated-value setter takes a value and optional `{ animate, duration, "
                  "easing, spring, onComplete }`",
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

// `r.focus()` / `r.blur()` on a `useRef` name (issue #226). The ref has no `.current`: a ref is the
// node it is attached to.
std::optional<ir::Action> lower_ref_focus(HookState& state, const jsx::Expr& call,
                                          const jsx::Expr& callee) {
    const rules::Symbol* ref = state.context.analysis.find(callee.operands[0]->text);
    if (ref == nullptr || ref->kind != rules::SymbolKind::Ref) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED", kBodyRule, call.span);
        return std::nullopt;
    }
    if (call.operands.size() != 1) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED", "`" + callee.text + "()` takes no arguments",
                  call.span, kBodyRule);
        return std::nullopt;
    }
    ir::Action action;
    action.kind = callee.text == "focus" ? ir::ActionKind::Focus : ir::ActionKind::Blur;
    action.target = "ref::" + ref->name;
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
        const jsx::Expr* statement_ptr = arrow.operands[i].get();
        // `guard && call(...)`: the call runs only when the pure guard holds (issue #236).
        ir::ExprPtr guard;
        if (statement_ptr != nullptr && statement_ptr->kind == jsx::ExprKind::Binary &&
            statement_ptr->text == "&&" && statement_ptr->operands.size() == 2 &&
            statement_ptr->operands[0] && statement_ptr->operands[1]) {
            guard = lower_component_expression(state, *statement_ptr->operands[0], names);
            if (state.error || !guard) {
                return actions;
            }
            statement_ptr = statement_ptr->operands[1].get();
        }
        if (statement_ptr == nullptr || statement_ptr->kind != jsx::ExprKind::Call ||
            statement_ptr->operands.empty() || !statement_ptr->operands[0]) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", kBodyRule,
                      statement_ptr ? statement_ptr->span : arrow.span);
            return actions;
        }
        const jsx::Expr* const statement = statement_ptr;
        const jsx::Expr& callee = *statement->operands[0];
        std::optional<ir::Action> action;
        if (callee.kind == jsx::ExprKind::Member &&
            (callee.text == "focus" || callee.text == "blur") && callee.operands.size() == 1 &&
            callee.operands[0] && callee.operands[0]->kind == jsx::ExprKind::Identifier) {
            action = lower_ref_focus(state, *statement, callee);
        } else if (callee.kind == jsx::ExprKind::Member && callee.text == "set" &&
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
        action->guard = std::move(guard);
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
