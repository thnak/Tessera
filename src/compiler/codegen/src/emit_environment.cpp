// SPDX-License-Identifier: MIT

#include "emit_environment.hpp"

#include <string>

namespace tessera::compiler::codegen::internal {

bool has_environment(const ir::Component& component) {
    return !component.environment.empty();
}

std::string environment_cpp_type(const ir::Environment& env) {
    // Every environment provider today returns a string: the size class (`compact`/`medium`/
    // `expanded`) and the theme id. A future numeric provider would add its mapping here, which is
    // why the type is a function rather than a literal at the call site.
    (void)env;
    return "std::string";
}

std::string environment_initial_value(const ir::Environment& env) {
    if (env.provider == "breakpoint") {
        // The narrow layout is the safe first frame: a component that branches on the class renders
        // its compact layout until the host pushes the real one, which is the same rule
        // `responsive::size_class_for` applies to an unmeasured width.
        return "\"compact\"";
    }
    if (env.provider == "theme") {
        // The engine always has a built-in theme with id `default` (jsx-subset-v1.md section 7.1),
        // so the field starts at a value the host would also report.
        return "\"default\"";
    }
    return "std::string{}";
}

void emit_environment_declarations(CppWriter& writer, const ir::Component& component,
                                   const ir::IrModule& module) {
    if (!has_environment(component)) {
        return;
    }
    (void)module;
    writer.line("// Environment values (useBreakpoint/useTheme): engine-owned, read by this");
    writer.line("// component. The host pushes a new value when the window or theme changes; the");
    writer.line("// field is private so only the generated setter can move it.");
    for (const ir::Environment& env : component.environment) {
        writer.line(environment_cpp_type(env) + " " + member_name(env.id) + " = " +
                    environment_initial_value(env) + ";");
    }
    for (const ir::Environment& env : component.environment) {
        writer.line("void set_environment_" + env.name + "(" + environment_cpp_type(env) +
                    " value);");
    }
}

void emit_environment_definitions(CppWriter& writer, const ir::Component& component,
                                  const ir::IrModule& module) {
    if (!has_environment(component)) {
        return;
    }
    (void)module;
    for (const ir::Environment& env : component.environment) {
        writer.open("void " + component.name + "::set_environment_" + env.name + "(" +
                    environment_cpp_type(env) + " value)");
        // Equality-gated like a state setter (ADR-041 decision 6): a host that pushes the same
        // value on every resize costs nothing, and a component that reads the value is not
        // re-rendered.
        writer.open("if (" + member_name(env.id) + " == value)");
        writer.line("return;");
        writer.close("");
        writer.line(member_name(env.id) + " = std::move(value);");
        writer.line("dirty_.set(kBit_" + member_name(env.id) + ");");
        writer.line("schedule_update();");
        writer.close("");
        writer.line("");
    }
}

} // namespace tessera::compiler::codegen::internal
