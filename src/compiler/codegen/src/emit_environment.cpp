// SPDX-License-Identifier: MIT

#include "emit_environment.hpp"

#include <string>

namespace tessera::compiler::codegen::internal {

bool has_environment(const ir::Component& component) {
    return !component.environment.empty();
}

std::string environment_cpp_type(const ir::Environment& env) {
    // A `media_query` provider returns a boolean (does the query match this window?), so its field
    // is a `bool`; every other provider today returns a string (the size class, the theme id, the
    // `useId` value). A future numeric provider would add its mapping here, which is why the type
    // is a function rather than a literal at the call site.
    if (env.provider == "media_query") {
        return "bool";
    }
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
    if (env.provider == "media_query") {
        // A query that has not been evaluated yet does not match, which is the same answer a
        // malformed query gives: a component renders its non-matching layout for the first frame
        // rather than claiming a match it has not checked.
        return "false";
    }
    if (env.provider == "id") {
        // `useId` (ADR-066 item 1): a stable id from the instance key and the call ordinal. The
        // generated base computes it in its constructor, because the instance key is set before
        // build() and the ordinal is the declaration order, so the value is fixed for the life of
        // the instance and identical across a re-render.
        return "{}";
    }
    return "std::string{}";
}

void emit_environment_declarations(CppWriter& writer, const ir::Component& component,
                                   const ir::IrModule& module) {
    if (!has_environment(component)) {
        return;
    }
    (void)module;
    writer.line("// Environment values (useBreakpoint/useTheme/useMediaQuery): engine-owned, read");
    writer.line("// by this component. The host pushes a new value when the window or theme");
    writer.line("// changes; the field is private so only the generated setter can move it.");
    for (const ir::Environment& env : component.environment) {
        writer.line(environment_cpp_type(env) + " " + member_name(env.id) + " = " +
                    environment_initial_value(env) + ";");
        if (env.provider == "media_query") {
            // The query is parsed once, not per frame: the matcher holds the parsed list and the
            // host only re-evaluates it against the new media context.
            writer.line("tessera::ui::responsive::MediaQueryMatcher " + member_name(env.id) +
                        "matcher_;");
        }
    }
    for (const ir::Environment& env : component.environment) {
        writer.line("void set_environment_" + env.name + "(" + environment_cpp_type(env) +
                    " value);");
        if (env.provider == "media_query") {
            // The host hands the window's media context; the component evaluates its own parsed
            // query against it, so the hook and the cascade share one evaluator (issue #631).
            writer.line("void set_environment_" + env.name +
                        "_context(const tessera::ui::style::MediaContext& context);");
            // The query is parsed once per component update, not per frame.
            writer.line("void parse_environment_" + env.name + "();");
            continue;
        }
        if (env.provider == "id") {
            // `useId` is not host-pushed: the value is derived from the instance identity, so it is
            // computed once in the constructor and never set from outside.
            continue;
        }
        // The `EnvironmentConsumer::Fn` thunk: a plain function pointer plus the component, so the
        // set needs no `std::function` and no RTTI (ADR-024).
        writer.line("static void set_environment_" + env.name +
                    "_thunk(void* context, std::string_view value);");
    }
    // The host-facing registration, mirroring `attach_scheduler`/`attach_layout_observers`: the
    // component joins the window's set at mount and leaves it at unmount, so a resize reaches it
    // without the host knowing what a component is.
    writer.line("void attach_environment(tessera::ui::responsive::EnvironmentSet& values);");
    writer.line("void detach_environment(tessera::ui::responsive::EnvironmentSet& values);");
}

bool emit_environment_definitions(CppWriter& writer, const ir::Component& component,
                                  const ir::IrModule& module, std::string& error) {
    if (!has_environment(component)) {
        return true;
    }
    (void)module;
    // A media-query hook parses its query once, at build time, from the IR argument. A query built
    // from state re-parses when the component updates, which is why the parse lives in
    // `apply_state()` rather than in a constructor.
    for (const ir::Environment& env : component.environment) {
        if (env.provider != "media_query") {
            continue;
        }
        const ExprEmission query = emit_expr(env.argument, component);
        if (!query.error.empty()) {
            error = "media query `" + env.id + "`: " + query.error;
            return false;
        }
        writer.open("void " + component.name + "::parse_environment_" + env.name + "()");
        writer.line(member_name(env.id) + "matcher_.parse(" + query.text + ");");
        writer.close("");
        writer.line("");
    }
    for (const ir::Environment& env : component.environment) {
        if (env.provider == "media_query") {
            // A media query is not pushed as a value: the host hands the component the window's
            // media context and the component evaluates its own parsed query against it. That is
            // what keeps the hook and the cascade on one evaluator (issue #631): the same
            // `style::evaluate` the stylesheet calls.
            writer.open("void " + component.name + "::set_environment_" + env.name +
                        "_context(const tessera::ui::style::MediaContext& context)");
            writer.line("set_environment_" + env.name + "(" + member_name(env.id) +
                        "matcher_.matches(context));");
            writer.close("");
            writer.line("");
            continue;
        }
        if (env.provider == "id") {
            // `useId` (ADR-066 item 1): the value is derived from the instance identity, so it has
            // no setter body. The declaration exists so the header's surface is uniform; nothing
            // calls it, because the value never changes.
            writer.open("void " + component.name + "::set_environment_" + env.name +
                        "(std::string value)");
            writer.line(
                "(void)value; // `useId` is derived from the instance identity, not pushed");
            writer.close("");
            writer.line("");
            continue;
        }
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
    // The bool setter a media-query hook shares with every other provider: equality-gated, so a
    // resize that does not cross the query costs nothing.
    for (const ir::Environment& env : component.environment) {
        if (env.provider != "media_query") {
            continue;
        }
        writer.open("void " + component.name + "::set_environment_" + env.name + "(bool value)");
        writer.open("if (" + member_name(env.id) + " == value)");
        writer.line("return;");
        writer.close("");
        writer.line(member_name(env.id) + " = value;");
        writer.line("dirty_.set(kBit_" + member_name(env.id) + ");");
        writer.line("schedule_update();");
        writer.close("");
        writer.line("");
    }
    // One consumer per value, each with its own thunk so the provider and the field are fixed at
    // compile time (no map lookup, no string compare on the frame path). A media-query hook has no
    // string push, so it registers no consumer: the host calls its context setter directly. A
    // `useId` value is derived from the instance identity, so it has no provider to join either.
    for (const ir::Environment& env : component.environment) {
        if (env.provider == "media_query" || env.provider == "id") {
            continue;
        }
        writer.open("void " + component.name + "::set_environment_" + env.name +
                    "_thunk(void* context, std::string_view value)");
        writer.line("static_cast<" + component.name + "*>(context)->set_environment_" + env.name +
                    "(std::string(value));");
        writer.close("");
        writer.line("");
    }
    writer.open("void " + component.name +
                "::attach_environment(tessera::ui::responsive::EnvironmentSet& values)");
    for (const ir::Environment& env : component.environment) {
        if (env.provider == "media_query" || env.provider == "id") {
            continue;
        }
        writer.line("values.attach(tessera::ui::responsive::EnvironmentConsumer{\"" + env.provider +
                    "\", &" + component.name + "::set_environment_" + env.name + "_thunk, this});");
    }
    writer.close("");
    writer.line("");
    writer.open("void " + component.name +
                "::detach_environment(tessera::ui::responsive::EnvironmentSet& values)");
    for (const ir::Environment& env : component.environment) {
        if (env.provider == "media_query" || env.provider == "id") {
            continue;
        }
        writer.line("values.detach(tessera::ui::responsive::EnvironmentConsumer{\"" + env.provider +
                    "\", &" + component.name + "::set_environment_" + env.name + "_thunk, this});");
    }
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
