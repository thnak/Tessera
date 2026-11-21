// SPDX-License-Identifier: MIT

#include "emit_behaviors.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

// The C++ member name of a behavior: its IR id without the `beh::` prefix, so `beh::press` becomes
// `press_`. The trailing underscore is what makes it a private field under the ADR-040
// partitioning.
std::string behavior_member(const ir::Behavior& behavior) {
    return member_name(behavior.id);
}

// The engine header a behavior's registry name lives in. The registry is one header, so this is the
// same for every behavior; it is a function so a future per-behavior header needs no call-site
// change.
std::string behavior_header(const ir::Behavior& /*behavior*/) {
    return "tessera/ui/behavior/registry.hpp";
}

} // namespace

bool uses_behaviors(const ir::Component& component) {
    return !component.behaviors.empty();
}

std::vector<std::string> behavior_headers(const ir::Component& component) {
    std::vector<std::string> headers;
    for (const ir::Behavior& behavior : component.behaviors) {
        headers.push_back(behavior_header(behavior));
    }
    std::sort(headers.begin(), headers.end());
    headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
    return headers;
}

bool emit_behavior_fields(CppWriter& writer, const ir::Component& component,
                          std::string& /*error*/) {
    if (!uses_behaviors(component)) {
        return true;
    }
    writer.line(
        "// ADR-064 item 7: the behavior instances. Each is attached after its target node");
    writer.line("// exists, updated when its hashed value options change, and detached in reverse");
    writer.line("// order at unmount. The rules live in the engine, not here.");
    for (const ir::Behavior& behavior : component.behaviors) {
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
    if (!uses_behaviors(component)) {
        return true;
    }
    for (const ir::Behavior& behavior : component.behaviors) {
        const std::string member = behavior_member(behavior);
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
                             std::string& /*error*/) {
    if (!uses_behaviors(component)) {
        return true;
    }
    writer.line("// The behavior outputs, read by the expressions that reference them. Before the");
    writer.line("// first attach and after detach each holds its manifest default.");
    for (const ir::Behavior& behavior : component.behaviors) {
        const std::string member = behavior_member(behavior);
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

bool emit_behavior_wiring(CppWriter& writer, const ir::Component& component,
                          std::string& error) {
    if (!uses_behaviors(component)) {
        return true;
    }
    // The attach pass. Every target ref must hold a node before the behavior attaches, which is the
    // spec's rule (section 8, "conditional target"): a behavior is never half-attached, and a target
    // that did not resolve (a conditional branch that did not take, an empty list) leaves the
    // behavior detached rather than attached to nothing.
    writer.open("void " + component.name +
                "::attach_behaviors(tessera::ui::InputRouter& router)");
    writer.line("const tessera::ui::behavior::Services services{&router};");
    for (const ir::Behavior& behavior : component.behaviors) {
        const std::string member = behavior_member(behavior);
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
        writer.line(nodes + ";");
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
        writer.line("this->" + behavior_member(component.behaviors[index]) + ".detach();");
    }
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
