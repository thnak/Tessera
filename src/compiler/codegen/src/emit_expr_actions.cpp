// SPDX-License-Identifier: MIT

// IR actions -> C++ handler statements (issue #327: split out of `emit_expr.cpp`, which had grown
// past the 500-line hard limit).
//
// An action is one side effect a handler performs (`setOpen(false)`, `anim::fade.set(1)`), and this
// is the whole of its C++ spelling: focus/blur, callback props, navigation, state and animation
// sets, and the guarded-statement wrapper. It lives beside the expression emitter rather than in it
// because it is a different job - actions emit *statements* into a handler body, expressions emit
// an rvalue - and the two only meet at `emit_expr`.

#include "binding_reads.hpp"
#include "codegen_internal.hpp"
#include "emit_behaviors.hpp"
#include "emit_dirty.hpp"
#include "emit_events.hpp"
#include "emit_resources.hpp"
#include "emit_types_header.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::codegen::internal {
// `e.width`, `e.contentHeight`, ...: the fields of a SizeChangeEvent parameter, spelled the way the
// C++ record names them, so an expression like `e.width >= 600` has a C++ form.
namespace {
LocalBindings payload_field_locals(std::span<const ir::Param> params) {
    LocalBindings locals;
    for (const ir::Param& param : params) {
        if (param.type == "KeyEvent" && !param.name.empty()) {
            locals.push_back({param.name + ".key", param.name, "KeyName"});
            continue;
        }
        if (param.type != "SizeChangeEvent" || param.name.empty()) {
            continue;
        }
        locals.push_back({param.name + ".width", param.name + ".width", "float"});
        locals.push_back({param.name + ".height", param.name + ".height", "float"});
        locals.push_back({param.name + ".contentWidth", param.name + ".content_width", "float"});
        locals.push_back({param.name + ".contentHeight", param.name + ".content_height", "float"});
    }
    return locals;
}
// The declared record a state cell's type is exactly (`Form`, not `list<Form>`), or null. A
// wrapper-typed cell keeps one bit: a dotted ref cannot index it.
const ir::RecordType* find_record_type_for_action(std::string_view ir_type,
                                                  std::span<const ir::RecordType> records) {
    for (const ir::RecordType& record : records) {
        if (record.name == ir_type) {
            return &record;
        }
    }
    return nullptr;
}

// A `set` of a record-typed state cell: compare field by field, raise the bits of the fields that
// changed, then assign. `value` is the C++ text of an expression of exactly this record type.
//
//   Form tessera_next = <value>;
//   if (!(state_form_.name == tessera_next.name)) { dirty_.set(kBit_state_form_name_); }
//   ... (one guarded set per member)
//   state_form_ = std::move(tessera_next);
//
// A `recordUpdate` writes exactly the fields it lists, so only those are compared: an unwritten
// field is copied from the current value and can never differ, and emitting its compare would be
// dead code (issue #834). A whole-record `set` passes `written` empty and compares every field. A
// NaN field always compares unequal, the safe direction (ADR-041 decision 6).
void emit_record_set(std::string_view cell_id, const ir::RecordType& record,
                     std::span<const ir::RecordType> records, const std::string& value,
                     std::span<const ir::RecordField> written, std::string& body) {
    const std::string member = member_name(cell_id);
    const std::vector<std::string> bits = cell_bit_ids(cell_id, record.name, records);
    body += "{\n";
    body += "const " + record.name + " tessera_next = " + value + ";\n";
    for (const ir::RecordMember& field : record.members) {
        if (!written.empty()) {
            bool is_written = false;
            for (const ir::RecordField& w : written) {
                if (w.name == field.name) {
                    is_written = true;
                    break;
                }
            }
            if (!is_written) {
                continue;
            }
        }
        body += "if (!(" + member + "." + field.name + " == tessera_next." + field.name + ")) {\n";
        // Raise only the leaf bits below this member (a member is a leaf itself when it is not a
        // plain record; a record member expands to its leaves).
        const std::string prefix = std::string(cell_id) + "." + field.name;
        for (const std::string& bit : bits) {
            if (bit == prefix || bit.rfind(prefix + ".", 0) == 0) {
                body += "dirty_.set(" + dirty_bit_name(bit) + ");\n";
            }
        }
        body += "}\n";
    }
    body += member + " = std::move(tessera_next);\n";
    body += "}\n";
}

// One action as C++ statements appended to `body`; false with `error` set on failure. `records` is
// the module's record type table: a `set` of a record-typed state splits its dirty bit into one per
// changed field (ADR-056 decision 1, #834).
bool emit_one_action(const ir::Action& action, const ir::Component& component,
                     std::span<const ir::Param> params, std::span<const ir::RecordType> records,
                     std::string& body, std::string& error) {
    if (action.kind == ir::ActionKind::Focus || action.kind == ir::ActionKind::Blur) {
        // The component reaches the scene's FocusManager through `set_focus_manager`; before
        // the host provides one (or before build() assigns the ref) the call is a no-op.
        const std::string node = member_name(action.target);
        body += "if (focus_manager_ != nullptr && " + node + " != nullptr) {\n";
        if (action.kind == ir::ActionKind::Focus) {
            body += "focus_manager_->request_focus(" + node + ");\n";
        } else {
            body += "if (focus_manager_->focused() == " + node +
                    ") {\nfocus_manager_->clear_focus();\n}\n";
        }
        body += "}\n";
        return true;
    }
    if (action.kind == ir::ActionKind::CallProp) {
        return emit_call_prop(action, component, params, body, error);
    }
    // ADR-066 item 4: a `dispatch({ type: "x" })` runs the handler the reducer's dispatch table
    // maps that literal to. Which handler it is decided at generation time, so there is no run-time
    // lookup and no payload: the generated call is the case's own method.
    if (action.kind == ir::ActionKind::Dispatch) {
        const ir::State* cell = nullptr;
        for (const ir::State& candidate : component.state) {
            if (candidate.id == action.target) {
                cell = &candidate;
            }
        }
        if (cell == nullptr) {
            error = "a dispatch names `" + action.target + "`, which is not a state variable";
            return false;
        }
        const ir::DispatchCase* entry = nullptr;
        for (const ir::DispatchCase& candidate : cell->dispatch) {
            if (candidate.action == action.action) {
                entry = &candidate;
            }
        }
        if (entry == nullptr) {
            error =
                "action `" + action.action + "` is not a case of `" + action.target + "`'s reducer";
            return false;
        }
        body += method_name(entry->handler) + "(tessera::ui::Event{});\n";
        return true;
    }
    // ADR-050 section 3.1 item 4 (issue #177): `nav.push(route)` calls the requirement's typed
    // accessor, so it is the cached pointer attach_services() resolved at mount: no lookup, no
    // registry. Each argument is an ordinary expression, so it reads props, state and the handler's
    // payload exactly as a setter's value does.
    if (action.kind == ir::ActionKind::CallService) {
        const ir::Service* service = nullptr;
        for (const ir::Service& candidate : component.services) {
            if (candidate.id == action.target) {
                service = &candidate;
            }
        }
        if (service == nullptr) {
            error = "a service call names `" + action.target + "`, which is not a requirement";
            return false;
        }
        std::string call = service->name + "()." + action.method + "(";
        for (std::size_t index = 0; index < action.args.size(); ++index) {
            const ExprEmission argument =
                emit_expr(*action.args[index], component, payload_field_locals(params));
            if (!argument.error.empty()) {
                error = argument.error;
                return false;
            }
            call += (index == 0 ? "" : ", ") + argument.text;
        }
        body += call + ");\n";
        return true;
    }
    if (!action.value) {
        error = "an action has no value expression";
        return false;
    }
    ExprEmission value;
    const ir::Expr& value_expr = *action.value;
    const std::size_t dot = value_expr.id.find('.');
    if (value_expr.kind == ir::ExprKind::Ref && dot != std::string::npos) {
        const std::string_view base = std::string_view(value_expr.id).substr(0, dot);
        const std::string_view field = std::string_view(value_expr.id).substr(dot + 1);
        for (const ir::Param& param : params) {
            if (field == "value" && param.name == base && param.type == "TextChangeEvent") {
                value.text = "std::string(" + std::string(base) + ".value)";
                break;
            }
        }
    }
    if (value.text.empty()) {
        value = emit_expr(*action.value, component, payload_field_locals(params));
    }
    if (!value.error.empty()) {
        error = value.error;
        return false;
    }
    const std::string_view target = action.target;
    if (action.kind == ir::ActionKind::Set && target.rfind(kStatePrefix, 0) == 0) {
        // ADR-067 item 4: a transition-marked setter starts the transition under the resource's
        // current epoch *before* it assigns, so `isPending` is true from the moment the change is
        // applied. The mark is inert without a `Suspense`/transition surface, which is the default.
        if (action.transition) {
            // A deferred value settles on what the UI showed before the change, so it reads (and
            // remembers) its argument while nothing is pending.
            for (const std::string& call : deferred_accessor_calls(component)) {
                body += "(void)" + call + ";\n";
            }
            if (uses_resources(component)) {
                // #656: the change starts a fetch for the new key, which the host performs and
                // which bumps the resource epoch by exactly one; the mark waits for that epoch.
                std::size_t index = 0;
                if (!transition_resource(component, action.target, index, error)) {
                    return false;
                }
                body += "pending_transition_.begin(" +
                        resource_view_member(component.resources[index]) + ".read().epoch + 1);\n";
            } else {
                body += "pending_transition_.begin(pending_resource_.epoch());\n";
            }
        }
        // A record-typed state cell splits into one bit per read leaf path (ADR-056 decision 1,
        // #834): a `set` of `{...state, name: x}` writes only the changed field's bit, and a
        // whole-record `set` compares field by field. Every other cell keeps its single bit.
        for (const ir::State& state : component.state) {
            if (state.id == target) {
                if (const ir::RecordType* record =
                        find_record_type_for_action(state.type, records)) {
                    const bool is_update =
                        action.value != nullptr && action.value->kind == ir::ExprKind::RecordUpdate;
                    emit_record_set(state.id, *record, records, value.text,
                                    is_update
                                        ? std::span<const ir::RecordField>(action.value->fields)
                                        : std::span<const ir::RecordField>(),
                                    body);
                    return true;
                }
                break;
            }
        }
        body += member_name(target) + " = " + value.text + ";\n";
        // ADR-056: a handler's write marks the cell dirty even when the value is unchanged, so the
        // apply_state() it ends with re-evaluates (a controlled input snaps back this way).
        body += "dirty_.set(kBit_" + member_name(target) + ");\n";
        return true;
    }
    if (action.kind == ir::ActionKind::AnimSet && target.rfind(kAnimPrefix, 0) == 0) {
        // The controller moves the sampled field toward the value over the frame ticks; a snap
        // (`animate: false`, a zero duration) lands in the field at once. Timing is literal in
        // the IR, so the easing is parsed once per call site.
        const std::string controller = animated_controller_name(target);
        body += "{\n";
        if (animated_has_completion(component, target)) {
            // Which handler runs when THIS set completes; a set without one clears it, so a
            // superseded animation never fires the old callback.
            std::size_t handler_index = 0;
            for (std::size_t i = 0; i < component.handlers.size(); ++i) {
                if (!action.on_complete.empty() && component.handlers[i].id == action.on_complete) {
                    handler_index = i + 1;
                }
            }
            if (!action.on_complete.empty() && handler_index == 0) {
                error = "onComplete handler `" + action.on_complete + "` is not declared";
                return false;
            }
            body +=
                animated_on_complete_name(target) + " = " + std::to_string(handler_index) + ";\n";
        }
        if (!action.animate) {
            body += "tessera::ui::animation::ScalarOptions options;\noptions.animate = false;\n";
        } else if (action.spring.has_value()) {
            const ir::SpringTiming& spring = *action.spring;
            body += "tessera::ui::animation::ScalarOptions options;\n"
                    "options.spring = tessera::ui::animation::SpringConfig{};\n"
                    "options.spring->stiffness = static_cast<float>(" +
                    literal_text(spring.stiffness) +
                    ");\n"
                    "options.spring->damping = static_cast<float>(" +
                    literal_text(spring.damping) +
                    ");\n"
                    "options.spring->mass = static_cast<float>(" +
                    literal_text(spring.mass) + ");\n";
        } else if (action.duration_ms.has_value() || !action.easing.empty()) {
            body += "tessera::ui::animation::ScalarOptions options;\n";
            if (action.duration_ms.has_value()) {
                body +=
                    "options.duration_s = " + literal_text(*action.duration_ms / 1000.0) + ";\n";
            }
            if (!action.easing.empty()) {
                body += "static const tessera::ui::animation::Easing easing =\n"
                        "tessera::ui::animation::parse_easing(\"" +
                        action.easing + "\").easing;\noptions.easing = easing;\n";
            }
        } else {
            body += "const tessera::ui::animation::ScalarOptions options;\n";
        }
        body += controller + ".set(static_cast<double>(" + value.text + "), options);\n" +
                member_name(target) + " = " + controller + ".value();\n" + "dirty_.set(kBit_" +
                member_name(target) + ");\n" + "if (" + controller + ".active() || " + controller +
                ".completion_pending()) {\nschedule_update();\n}\n}\n";
        return true;
    }
    error = "unsupported action target `" + action.target + "`";
    return false;
}

} // namespace

std::optional<std::string> emit_actions(const std::vector<ir::Action>& actions,
                                        const ir::Component& component,
                                        std::span<const ir::Param> params,
                                        std::span<const ir::RecordType> records,
                                        std::string& error) {
    std::string body;
    for (const ir::Action& action : actions) {
        // ADR-066 item 2: an `If` action is a C++ `if`/`else` whose branches are emitted by this
        // same function, so nesting needs no special case. Its `guard` is the condition, which is
        // why it is handled before the guarded-statement path below.
        if (action.kind == ir::ActionKind::If) {
            if (!action.guard) {
                error = "an `if` action has no condition";
                return std::nullopt;
            }
            const ExprEmission condition =
                emit_expr(*action.guard, component, payload_field_locals(params));
            if (!condition.error.empty()) {
                error = condition.error;
                return std::nullopt;
            }
            std::optional<std::string> then_body =
                emit_actions(action.then_actions, component, params, records, error);
            if (!then_body) {
                return std::nullopt;
            }
            body += "if (" + condition.text + ") {\n" + *then_body + "}";
            if (!action.else_actions.empty()) {
                std::optional<std::string> else_body =
                    emit_actions(action.else_actions, component, params, records, error);
                if (!else_body) {
                    return std::nullopt;
                }
                body += " else {\n" + *else_body + "}";
            }
            body += "\n";
            continue;
        }
        std::string one;
        if (!emit_one_action(action, component, params, records, one, error)) {
            return std::nullopt;
        }
        if (!action.guard) {
            body += one;
            continue;
        }
        // A guarded statement (`e.key === "Escape" && setOpen(false)`, issue #236) runs only when
        // its pure guard holds.
        const ExprEmission guard =
            emit_expr(*action.guard, component, payload_field_locals(params));
        if (!guard.error.empty()) {
            error = guard.error;
            return std::nullopt;
        }
        body += "if (" + guard.text + ") {\n" + one + "}\n";
    }
    return body;
}

} // namespace tessera::compiler::codegen::internal
