// SPDX-License-Identifier: MIT

// Inline setter handlers (docs/specs/jsx-subset-v1.md section 9.2, docs/specs/ir-v1.md section
// 2.1): the statements of a setter-call arrow become ordered IR actions.

#include "handler_control_internal.hpp"
#include "hook_lowering_internal.hpp"
#include "tessera/schema/intrinsics.hpp"

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

} // namespace

// The four call forms a handler body may hold. They are outside the anonymous namespace because the
// control-flow walk in `handler_control.cpp` reaches them through `lower_call_action`
// (`handler_control_internal.hpp` declares them).
std::optional<ir::Action> lower_setter_call(HookState& state, const jsx::Expr& call,
                                            const jsx::Expr& callee, ComponentNames& names) {
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
                                             const jsx::Expr& callee, ComponentNames& names) {
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

// `onPress()` / `onChange(e)`: a call of a callback prop of the component (issue #286). The payload
// argument is the handler's own parameter; its record type is checked against the prop later, by
// `check_callback_calls`, once the handler's parameter types are known.
std::optional<ir::Action> lower_callback_call(HookState& state, const jsx::Expr& call,
                                              const jsx::Expr& callee, const jsx::Expr& arrow) {
    const rules::Symbol* prop = state.context.analysis.find(callee.text);
    if (prop == nullptr || prop->kind != rules::SymbolKind::Prop) {
        return std::nullopt;
    }
    if (call.operands.size() > 2) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "callback prop `" + callee.text + "` takes at most one argument", call.span,
                  kBodyRule);
        return std::nullopt;
    }
    ir::Action action;
    action.kind = ir::ActionKind::CallProp;
    action.target = "prop::" + prop->name;
    if (call.operands.size() == 2) {
        const jsx::Expr* argument = call.operands[1].get();
        bool is_param = false;
        if (argument != nullptr && argument->kind == jsx::ExprKind::Identifier) {
            for (const std::string& param : arrow.params) {
                is_param = is_param || param == argument->text;
            }
        }
        if (!is_param) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                      "callback prop `" + callee.text +
                          "` is called with the handler's own parameter (the event payload)",
                      call.span, kBodyRule);
            return std::nullopt;
        }
        action.value = ir::make_ref(argument->text);
    }
    return action;
}

//   * `if (cond) { ... } else { ... }` becomes one `If` action whose `guard` is the condition and
//     whose two lists are the branches. Nesting is an `If` inside either list, so a chain needs no
//     new kind.
//   * `const x = <PureExpr>;` is a local, inlined at each use exactly as a component-level `const`
//     is (decision 8), so it produces no action at all.
//   * an early `return;` ends the list it appears in. A `return` with a value is only meaningful in
//     a preview body, which `lower_preview` handles separately.
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
        if (statement_ptr == nullptr) {
            fail_hook(state, "TESSERA_IR_MALFORMED", "a handler statement is missing", arrow.span);
            return actions;
        }
        // `if`/`else` (ADR-066 item 2).
        if (statement_ptr->kind == jsx::ExprKind::If) {
            std::optional<ir::Action> branch = lower_if_action(state, *statement_ptr, arrow, names);
            if (state.error || !branch) {
                return actions;
            }
            actions.push_back(std::move(*branch));
            continue;
        }
        // `const x = <PureExpr>;` (ADR-066 item 2): a local, inlined at each use, so it contributes
        // no action. The name is registered so a later statement can read it.
        if (statement_ptr->kind == jsx::ExprKind::Const) {
            if (statement_ptr->operands.size() != 1 || !statement_ptr->operands[0]) {
                fail_hook(state, "TESSERA_IR_MALFORMED", "a handler local needs one initializer",
                          statement_ptr->span);
                return actions;
            }
            names.add_const_local(statement_ptr->text, statement_ptr->operands[0].get());
            continue;
        }
        // An early `return;` ends the list. A value is only allowed in a preview body, which
        // `lower_preview` handles; here it would have nowhere to go.
        if (statement_ptr->kind == jsx::ExprKind::Return) {
            if (!statement_ptr->operands.empty()) {
                fail_hook(
                    state, "TESSERA_IR_UNSUPPORTED",
                    "`return <value>` is only allowed as the last statement of a preview body",
                    statement_ptr->span, "write `return;` to leave the handler early");
                return actions;
            }
            break;
        }
        // `guard && call(...)`: the call runs only when the pure guard holds (issue #236).
        ir::ExprPtr guard;
        if (statement_ptr->kind == jsx::ExprKind::Binary && statement_ptr->text == "&&" &&
            statement_ptr->operands.size() == 2 && statement_ptr->operands[0] &&
            statement_ptr->operands[1]) {
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
        std::optional<ir::Action> action = lower_call_action(state, *statement_ptr, arrow, names);
        if (state.error) {
            return actions;
        }
        if (!action) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", kBodyRule, statement_ptr->span);
            return actions;
        }
        action->guard = std::move(guard);
        actions.push_back(std::move(*action));
    }
    return actions;
}

std::vector<ir::Action> lower_actions(HookState& state, const jsx::Expr& arrow) {
    if (arrow.kind != jsx::ExprKind::Arrow) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "only an arrow function has inline handler actions", arrow.span);
        return {};
    }
    return lower_statements(state, arrow, arrow.operands.size());
}

} // namespace tessera::compiler::lowering::internal

namespace tessera::compiler::lowering {

std::optional<jsx::Diagnostic> check_callback_calls(const HookLoweringContext& context,
                                                    std::span<const ir::Param> params,
                                                    std::span<const ir::Action> actions,
                                                    jsx::Span span) {
    internal::HookState state{context, std::nullopt};
    const auto violation = [&](const std::string& message) {
        internal::fail_hook(state, "TESSERA_IR_UNSUPPORTED", message, span);
        return state.error;
    };
    for (const ir::Action& action : actions) {
        if (action.kind != ir::ActionKind::CallProp) {
            continue;
        }
        const std::string name = action.target.substr(6);
        const ir::Prop* prop = nullptr;
        if (context.declared_props != nullptr) {
            for (const ir::Prop& candidate : *context.declared_props) {
                if (candidate.name == name) {
                    prop = &candidate;
                }
            }
        }
        if (prop == nullptr) {
            return violation("the type of prop `" + name +
                             "` is not known, so it cannot be called; declare it in the props "
                             "type as `() => void` or `(e: Record) => void`");
        }
        if (prop->type == "callback") {
            if (action.value) {
                return violation("callback prop `" + name + "` takes no argument");
            }
            continue;
        }
        if (prop->type == "children" || prop->type.rfind("optional<", 0) == 0 ||
            schema::find_type_record(prop->type) == nullptr) {
            return violation("prop `" + name + "` is not a callback");
        }
        bool matches = action.value != nullptr && action.value->kind == ir::ExprKind::Ref;
        if (matches) {
            matches = false;
            for (const ir::Param& param : params) {
                matches = matches || (param.name == action.value->id && param.type == prop->type);
            }
        }
        if (!matches) {
            return violation("callback prop `" + name + "` takes a `" + prop->type +
                             "` payload; call it with a handler parameter of that type");
        }
    }
    return std::nullopt;
}

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
