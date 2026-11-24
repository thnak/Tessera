// SPDX-License-Identifier: MIT

#include "emit_environment.hpp"

#include <charconv>
#include <optional>
#include <string>

namespace tessera::compiler::codegen::internal {

bool has_environment(const ir::Component& component) {
    return !component.environment.empty();
}

std::string environment_cpp_type(const ir::Environment& env,
                                 std::span<const ir::RecordType> records) {
    // A `media_query` provider returns a boolean (does the query match this window?), so its field
    // is a `bool`; every other engine provider today returns a string (the size class, the theme
    // id, the `useId` value). A future numeric provider would add its mapping here, which is why
    // the type is a function rather than a literal at the call site.
    if (env.provider == "media_query") {
        return "bool";
    }
    // ADR-066 item 5 (issue #802): a `context:` provider's value type is the context's, which may
    // name a declared record (`Theme`); the generated field is that struct. A primitive context
    // keeps the primitive's C++ form.
    if (env.provider.rfind("context:", 0) == 0) {
        if (const std::optional<std::string> cpp = cpp_type(env.type, records)) {
            return *cpp;
        }
        return "std::string";
    }
    return "std::string";
}

namespace {

// The default of a `context:` provider (ADR-066 item 5, issue #802): the environment entry carries
// the `createContext` value as its argument, and the generated field starts at it â€” the owner's
// decision that a consumer with no ancestor provider falls back to the default. A record emits as
// designated aggregate initialization (`Theme{.accent = "blue"}`), a literal as `{value}`.
bool context_initial_value(const ir::Environment& env, std::span<const ir::RecordType>,
                           std::string& out, std::string& error) {
    if (env.argument.kind == ir::ExprKind::Literal) {
        if (const auto* text = std::get_if<std::string>(&env.argument.literal)) {
            out = "\"" + *text + "\"";
            return true;
        }
        if (const auto* boolean = std::get_if<bool>(&env.argument.literal)) {
            out = *boolean ? "true" : "false";
            return true;
        }
        if (const auto* number = std::get_if<double>(&env.argument.literal)) {
            // The shortest round-trip C++ form, the same rule the expression emitter uses for a
            // float literal.
            char buffer[64];
            const std::to_chars_result result =
                std::to_chars(buffer, buffer + sizeof buffer, *number);
            out = std::string(buffer, result.ptr);
            if (out.find_first_of(".eE") == std::string::npos) {
                out += ".0";
            }
            return true;
        }
        out = "{}";
        return true;
    }
    // A record default: emitted with the same initializer emitter a prop default uses.
    if (env.argument.kind == ir::ExprKind::Record) {
        const ir::Component empty;
        const ExprEmission emission = emit_initializer_expr(env.argument, empty);
        if (!emission.error.empty()) {
            error = "the default of context `" + env.name + "`: " + emission.error;
            return false;
        }
        out = emission.text;
        return true;
    }
    error = "the default of context `" + env.name +
            "` is not a compile-time constant; V1 accepts a literal or an object literal";
    return false;
}

} // namespace

bool environment_initial_value(const ir::Environment& env, std::span<const ir::RecordType> records,
                               std::string& out, std::string& error) {
    if (env.provider.rfind("context:", 0) == 0) {
        return context_initial_value(env, records, out, error);
    }
    if (env.provider == "breakpoint") {
        // The narrow layout is the safe first frame: a component that branches on the class renders
        // its compact layout until the host pushes the real one, which is the same rule
        // `responsive::size_class_for` applies to an unmeasured width.
        out = "\"compact\"";
        return true;
    }
    if (env.provider == "theme") {
        // The engine always has a built-in theme with id `default` (jsx-subset-v1.md section 7.1),
        // so the field starts at a value the host would also report.
        out = "\"default\"";
        return true;
    }
    if (env.provider == "media_query") {
        // A query that has not been evaluated yet does not match, which is the same answer a
        // malformed query gives: a component renders its non-matching layout for the first frame
        // rather than claiming a match it has not checked.
        out = "false";
        return true;
    }
    if (env.provider == "id") {
        // `useId` (ADR-066 item 1): a stable id from the instance key and the call ordinal. The
        // generated base computes it in its constructor, because the instance key is set before
        // build() and the ordinal is the declaration order, so the value is fixed for the life of
        // the instance and identical across a re-render.
        out = "{}";
        return true;
    }
    out = "std::string{}";
    return true;
}

void emit_environment_declarations(CppWriter& writer, const ir::Component& component,
                                   const ir::IrModule& module) {
    if (!has_environment(component)) {
        return;
    }
    writer.line("// Environment values (useBreakpoint/useTheme/useMediaQuery): engine-owned, read");
    writer.line("// by this component. The host pushes a new value when the window or theme");
    writer.line("// changes; the field is private so only the generated setter can move it. A");
    writer.line("// `context:` provider's value (ADR-066 item 5, issue #802) is pushed by the");
    writer.line("// ancestor component that provides it, or starts at the context default.");
    for (const ir::Environment& env : component.environment) {
        std::string initial;
        std::string error;
        if (!environment_initial_value(env, module.types, initial, error)) {
            // A bad default is a compile error upstream; the declaration still has to exist, so
            // the field starts empty and the error surfaces where the initial value is demanded.
            writer.line(environment_cpp_type(env, module.types) + " " + member_name(env.id) +
                        "{};");
            continue;
        }
        writer.line(environment_cpp_type(env, module.types) + " " + member_name(env.id) + " = " +
                    initial + ";");
        if (env.provider == "media_query") {
            // The query is parsed once, not per frame: the matcher holds the parsed list and the
            // host only re-evaluates it against the new media context.
            writer.line("tessera::ui::responsive::MediaQueryMatcher " + member_name(env.id) +
                        "matcher_;");
        }
    }
    for (const ir::Environment& env : component.environment) {
        if (env.provider.rfind("context:", 0) == 0) {
            // ADR-066 item 5 (issue #802): the value is pushed by the providing ancestor through
            // this setter; no host registration and no string thunk exist for it.
            writer.line("void set_environment_" + env.name + "(" +
                        environment_cpp_type(env, module.types) + " value);");
            continue;
        }
        writer.line("void set_environment_" + env.name + "(" +
                    environment_cpp_type(env, module.types) + " value);");
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
        if (env.provider.rfind("context:", 0) == 0) {
            // ADR-066 item 5 (issue #802): a context value is pushed by the providing ancestor.
            // The gate is dropped for a record (no generated equality in this slice, ADR-051
            // section 5), kept for a primitive: the value is a pure function of the provider's
            // own state, so an unchanged re-push costs nothing.
            const bool record = env.argument.kind == ir::ExprKind::Record;
            writer.open("void " + component.name + "::set_environment_" + env.name + "(" +
                        environment_cpp_type(env, module.types) + " value)");
            if (!record) {
                writer.open("if (" + member_name(env.id) + " == value)");
                writer.line("return;");
                writer.close("");
            }
            writer.line(member_name(env.id) + " = std::move(value);");
            writer.line("dirty_.set(kBit_" + member_name(env.id) + ");");
            writer.line("schedule_update();");
            writer.close("");
            writer.line("");
            continue;
        }
        writer.open("void " + component.name + "::set_environment_" + env.name + "(" +
                    environment_cpp_type(env, module.types) + " value)");
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
        if (env.provider == "media_query" || env.provider == "id" ||
            env.provider.rfind("context:", 0) == 0) {
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
        if (env.provider == "media_query" || env.provider == "id" ||
            env.provider.rfind("context:", 0) == 0) {
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
        if (env.provider == "media_query" || env.provider == "id" ||
            env.provider.rfind("context:", 0) == 0) {
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
