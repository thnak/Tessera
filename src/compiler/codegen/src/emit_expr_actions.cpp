// SPDX-License-Identifier: MIT

// IR actions -> C++ handler statements (issue #327: split out of `emit_expr.cpp`, which had grown
// past the 500-line hard limit).
//
// An action is one side effect a handler performs (`setOpen(false)`, `anim::fade.set(1)`), and this
// is the whole of its C++ spelling: focus/blur, callback props, navigation, state and animation
// sets, and the guarded-statement wrapper. It lives beside the expression emitter rather than in it
// because it is a different job - actions emit *statements* into a handler body, expressions emit
// an rvalue - and the two only meet at `emit_expr`.

#include "codegen_internal.hpp"

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
// One action as C++ statements appended to `body`; false with `error` set on failure.
bool emit_one_action(const ir::Action& action, const ir::Component& component,
                     std::span<const ir::Param> params, std::string& body, std::string& error) {
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
        body += member_name(target) + " = " + value.text + ";\n";
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
                member_name(target) + " = " + controller + ".value();\n" + "if (" + controller +
                ".active() || " + controller +
                ".completion_pending()) {\nschedule_update();\n}\n}\n";
        return true;
    }
    error = "unsupported action target `" + action.target + "`";
    return false;
}

} // namespace

std::optional<std::string> emit_actions(const std::vector<ir::Action>& actions,
                                        const ir::Component& component,
                                        std::span<const ir::Param> params, std::string& error) {
    std::string body;
    for (const ir::Action& action : actions) {
        std::string one;
        if (!emit_one_action(action, component, params, one, error)) {
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
