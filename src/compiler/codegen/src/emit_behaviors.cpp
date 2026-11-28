// SPDX-License-Identifier: MIT

#include "emit_behaviors.hpp"
#include "emit_suspense.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

// `useTransition` and `useDeferredValue` are registered engine behaviors, but the generated base
// reads the component's own pending model for them (a `TransitionMark`, a `DeferredScalar`) because
// the marked setter and the resource subscription already drive that state; there is no scene
// target to attach to and nothing for an `Instance` slot to own.
bool model_backed_behavior(const ir::Behavior& behavior) {
    return behavior.behavior == "transition" || behavior.behavior == "deferred_value";
}

std::string deferred_member(const ir::Behavior& behavior) {
    return member_name(behavior.id) + "deferred_";
}

// The C++ member name of a behavior: its IR id without the `beh::` prefix, so `beh::press` becomes
// `press_`. The trailing underscore is what makes it a private field under the ADR-040
// partitioning.
std::string behavior_member(const ir::Behavior& behavior) {
    return member_name(behavior.id);
}

// The C++ type a deferred value stores, or empty when the IR type has none (a record or a list).
std::string deferred_cpp_type(const ir::Behavior& behavior) {
    if (behavior.outputs.empty()) {
        return {};
    }
    const std::optional<std::string> type = cpp_type(behavior.outputs.front().type);
    return type ? *type : std::string();
}

bool emit_deferred_accessor(CppWriter& writer, const ir::Component& component,
                            const ir::Behavior& behavior, std::string& error) {
    const std::string type = deferred_cpp_type(behavior);
    if (type.empty() || behavior.args.empty() || behavior.outputs.empty()) {
        error = "behavior `" + behavior.id + "` is not a scalar deferred value";
        return false;
    }
    const ExprEmission current = emit_expr(behavior.args.front().expr, component);
    if (!current.error.empty()) {
        error = "behavior `" + behavior.id + "` value: " + current.error;
        return false;
    }
    // `useDeferredValue` alone is never pending (pending-ui-v1.md decision 4): the lane is the
    // component's own transition mark, so without one the value is its argument.
    const std::string held =
        has_transition_surface(component) ? "pending_transition_.pending()" : "false";
    writer.line("[[nodiscard]] " +
                std::string(type == "std::string" ? "const std::string&" : type) + " " +
                member_name(behavior.id) + behavior.outputs.front().name + "() const {");
    writer.line("    return " + member_name(behavior.id) + "deferred_.read(" + current.text + ", " +
                held + ");");
    writer.line("}");
    return true;
}

// The engine header a behavior's registry name lives in. The registry is one header, so this is the
// same for every behavior; it is a function so a future per-behavior header needs no call-site
// change.
std::string behavior_header(const ir::Behavior& /*behavior*/) {
    return "tessera/ui/behavior/registry.hpp";
}

} // namespace

std::vector<std::string> deferred_accessor_calls(const ir::Component& component) {
    std::vector<std::string> calls;
    for (const ir::Behavior& behavior : component.behaviors) {
        if (behavior.behavior == "deferred_value" && !behavior.outputs.empty()) {
            calls.push_back(member_name(behavior.id) + behavior.outputs.front().name + "()");
        }
    }
    return calls;
}

bool uses_behaviors(const ir::Component& component) {
    for (const ir::Behavior& behavior : component.behaviors) {
        if (!model_backed_behavior(behavior)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> behavior_headers(const ir::Component& component) {
    std::vector<std::string> headers;
    for (const ir::Behavior& behavior : component.behaviors) {
        if (!model_backed_behavior(behavior)) {
            headers.push_back(behavior_header(behavior));
        }
    }
    std::sort(headers.begin(), headers.end());
    headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
    return headers;
}

bool emit_behavior_fields(CppWriter& writer, const ir::Component& component, std::string& error) {
    // A `useDeferredValue` keeps one settled scalar (pending-ui-v1.md section 6); a
    // `useTransition` keeps nothing, it reads the component's `TransitionMark`.
    for (const ir::Behavior& behavior : component.behaviors) {
        if (behavior.behavior != "deferred_value") {
            continue;
        }
        const std::string type = deferred_cpp_type(behavior);
        if (type.empty()) {
            error = "behavior `" + behavior.id + "` defers a value with no scalar C++ type";
            return false;
        }
        writer.line("// `" + behavior.id + "` (useDeferredValue): the last settled value.");
        writer.line("mutable tessera::ui::pending::DeferredScalar<" + type + "> " +
                    deferred_member(behavior) + ";");
    }
    if (!uses_behaviors(component)) {
        return true;
    }
    writer.line(
        "// ADR-064 item 7: the behavior instances. Each is attached after its target node");
    writer.line("// exists, updated when its hashed value options change, and detached in reverse");
    writer.line("// order at unmount. The rules live in the engine, not here.");
    for (const ir::Behavior& behavior : component.behaviors) {
        if (model_backed_behavior(behavior)) {
            continue; // read from the component's pending model, see the accessors
        }
        const std::string member = behavior_member(behavior);
        // The slot is the shared `Instance`, not the engine `Behavior`: the instance owns the
        // options hash and the attach/update/refresh/detach rules, so the generated base and the
        // preview session drive a behavior the same way (behavior-hooks-v1.md section 9).
        writer.line("tessera::ui::behavior::Instance " + member + ";");
        // The evaluated options, kept so `refresh_behaviors()` can recompute the outputs from the
        // scene's interaction state without re-evaluating an expression (behavior-hooks-v1.md
        // section 8: `refresh` does not consult the options hash).
        writer.line("std::vector<tessera::ui::behavior::ArgValue> args_" + member + ";");
    }
    return true;
}

bool claim_behavior_names(const ir::Component& component, std::set<std::string>& names,
                          std::string& error) {
    for (const ir::Behavior& behavior : component.behaviors) {
        const std::string member = behavior_member(behavior);
        if (behavior.behavior == "deferred_value" &&
            !names.insert(deferred_member(behavior)).second) {
            error = "behavior `" + behavior.id + "` collides with another generated member";
            return false;
        }
        if (!names.insert(member).second) {
            error = "behavior `" + behavior.id + "` collides with another generated member";
            return false;
        }
        // One accessor per output, named `<behavior>_<output>`, so a lowered `press.pressed` reads
        // a generated accessor rather than reaching into the instance.
        for (const ir::BehaviorOutput& output : behavior.outputs) {
            const std::string accessor = member + output.name;
            if (!names.insert(accessor).second) {
                error = "behavior output `" + behavior.id + "." + output.name +
                        "` collides with another generated member";
                return false;
            }
        }
    }
    return true;
}

bool emit_behavior_accessors(CppWriter& writer, const ir::Component& component,
                             std::string& error) {
    if (component.behaviors.empty()) {
        return true;
    }
    writer.line("// The behavior outputs, read by the expressions that reference them. Before the");
    writer.line("// first attach and after detach each holds its manifest default.");
    for (const ir::Behavior& behavior : component.behaviors) {
        const std::string member = behavior_member(behavior);
        if (behavior.behavior == "transition") {
            // pending-ui-v1.md section 5: `isPending` is the component's own transition mark.
            writer.line("[[nodiscard]] bool " + member + "isPending() const {");
            writer.line("    return pending_transition_.pending();");
            writer.line("}");
            continue;
        }
        if (behavior.behavior == "deferred_value") {
            if (!emit_deferred_accessor(writer, component, behavior, error)) {
                return false;
            }
            continue;
        }
        for (std::size_t index = 0; index < behavior.outputs.size(); ++index) {
            const ir::BehaviorOutput& output = behavior.outputs[index];
            const bool is_bool = output.type == "bool";
            const bool is_number = output.type == "number";
            const std::string type = is_bool ? "bool" : is_number ? "double" : "std::string_view";
            const std::string zero = is_bool ? "false" : is_number ? "0.0" : "{}";
            const std::string kind = is_bool ? "Bool" : is_number ? "Number" : "String";
            const std::string field = is_bool ? "boolean" : is_number ? "number" : "text";
            const std::string at = std::to_string(index);
            writer.line("[[nodiscard]] " + type + " " + member + output.name + "() const {");
            writer.line("    const auto outputs = " + member + ".outputs();");
            writer.line("    if (outputs.size() <= " + at + " || outputs[" + at +
                        "].kind != tessera::ui::behavior::OutputValue::Kind::" + kind + ") {");
            writer.line("        return " + zero + ";");
            writer.line("    }");
            writer.line("    return outputs[" + at + "]." + field + ";");
            writer.line("}");
        }
    }
    return true;
}

bool emit_behavior_wiring(CppWriter& writer, const ir::Component& component, std::string& error) {
    if (!uses_behaviors(component)) {
        return true;
    }
    // The attach pass. Every target ref must hold a node before the behavior attaches, which is the
    // spec's rule (section 8, "conditional target"): a behavior is never half-attached, and a
    // target that did not resolve (a conditional branch that did not take, an empty list) leaves
    // the behavior detached rather than attached to nothing.
    writer.open("void " + component.name + "::attach_behaviors(tessera::ui::InputRouter& router)");
    writer.line("const tessera::ui::behavior::Services services{&router};");
    for (const ir::Behavior& behavior : component.behaviors) {
        if (model_backed_behavior(behavior)) {
            continue;
        }
        const std::string member = behavior_member(behavior);
        // A behavior with no target (`useHotkeys`) has no node to wait for: it attaches at once,
        // which `Instance::attach` refuses on purpose for a behavior that declared targets and got
        // none.
        const bool targetless = behavior.targets.empty();
        // The target nodes, in manifest order. A ref member is a `Node*` the build pass assigned.
        std::string nodes = "const std::array<tessera::ui::Node*, " +
                            std::to_string(behavior.targets.size()) + "> targets_" + member + "{";
        for (std::size_t index = 0; index < behavior.targets.size(); ++index) {
            if (index != 0) {
                nodes += ", ";
            }
            nodes += member_name(behavior.targets[index]);
        }
        nodes += "}";
        if (!targetless) {
            writer.line(nodes + ";");
        }
        // The evaluated value options, in manifest order, into the member `refresh` reuses. A
        // callback arg carries its handler id; a value arg is evaluated here, because a behavior's
        // options are not re-evaluated per pointer move.
        writer.line("args_" + member + ".clear();");
        for (const ir::BehaviorArg& arg : behavior.args) {
            if (!arg.handler.empty()) {
                writer.line("args_" + member +
                            ".push_back(tessera::ui::behavior::make_handler_arg(0));");
                continue;
            }
            const ExprEmission value = emit_expr(arg.expr, component);
            if (!value.error.empty()) {
                error = "behavior `" + behavior.id + "` option `" + arg.name + "`: " + value.error;
                return false;
            }
            writer.line("args_" + member + ".push_back(tessera::ui::behavior::make_arg(" +
                        value.text + "));");
        }
        if (targetless) {
            writer.line("this->" + member + ".attach_untargeted(\"" + behavior.behavior +
                        "\", services, tessera::ui::behavior::ArgsView{args_" + member + "});");
            continue;
        }
        writer.line("this->" + member + ".attach(\"" + behavior.behavior +
                    "\", tessera::ui::behavior::Targets{targets_" + member +
                    "}, services, tessera::ui::behavior::ArgsView{args_" + member + "});");
    }
    writer.close("");
    writer.line("");
    // The scene-driven refresh: after an input dispatch the interaction state moved while the
    // component's options did not, so the outputs are recomputed without consulting the hash
    // (behavior-hooks-v1.md section 8, the `refresh` trigger).
    writer.open("void " + component.name + "::refresh_behaviors()");
    for (const ir::Behavior& behavior : component.behaviors) {
        if (model_backed_behavior(behavior)) {
            continue;
        }
        const std::string member = behavior_member(behavior);
        writer.line("if (this->" + member + ".attached()) {");
        ++writer.indent;
        writer.line("this->" + member + ".refresh(tessera::ui::behavior::ArgsView{args_" + member +
                    "});");
        --writer.indent;
        writer.line("}");
    }
    writer.close("");
    writer.line("");
    // Detach in reverse declaration order, as the spec requires (section 8, "order").
    writer.open("void " + component.name + "::detach_behaviors()");
    for (std::size_t index = component.behaviors.size(); index-- > 0;) {
        if (!model_backed_behavior(component.behaviors[index])) {
            writer.line("this->" + behavior_member(component.behaviors[index]) + ".detach();");
        }
    }
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
