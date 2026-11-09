// SPDX-License-Identifier: MIT

// Component lifecycle emission (docs/design/compiler.md section 11): `useEffect` deps-hash helpers
// plus mount/update_effects/unmount, and the ref-target assignment the generated build() performs.

#include "codegen_internal.hpp"
#include "emit_events.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::string effect_state_name(std::string_view effect_id) {
    return member_name(effect_id) + "state_";
}

const ir::Binding* find_binding(const std::vector<const ir::Binding*>& bindings,
                                std::string_view id) {
    for (const ir::Binding* binding : bindings) {
        if (binding->id == id) {
            return binding;
        }
    }
    return nullptr;
}

// Resolves one effect dependency to its generated member and C++ type.
bool dep_member(const ir::Component& component, std::string_view dep, std::string& member,
                std::string& type, std::string& error) {
    for (const ir::State& state : component.state) {
        if (state.id == dep) {
            const std::optional<std::string> mapped = cpp_type(state.type);
            if (!mapped) {
                return fail(error,
                            "effect dep `" + std::string(dep) + "` has a type with no C++ form");
            }
            member = member_name(state.id);
            type = *mapped;
            return true;
        }
    }
    for (const ir::Prop& prop : component.props) {
        if ("prop::" + prop.name != dep) {
            continue;
        }
        const std::optional<std::string> mapped = cpp_type(prop.type);
        if (!mapped) {
            return fail(error, "effect dep `" + std::string(dep) + "` has a type with no C++ form");
        }
        member = member_name("prop::" + prop.name);
        type = *mapped;
        return true;
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (value.id == dep) {
            const std::optional<std::string> mapped = cpp_type(value.type);
            if (!mapped) {
                return fail(error,
                            "effect dep `" + std::string(dep) + "` has a type with no C++ form");
            }
            member = member_name(value.id);
            type = *mapped;
            return true;
        }
    }
    return fail(error, "effect dep `" + std::string(dep) +
                           "` is not a state, prop or animated value reference");
}

bool emit_dep_hash(CppWriter& writer, const ir::Component& component, std::size_t index,
                   const ir::Effect& effect, std::string& error) {
    writer.line("std::uint64_t dep_hash_" + std::to_string(index) + "(const " + component.name +
                "& component) {");
    ++writer.indent;
    writer.line("std::uint64_t hash = tessera::ui::kEffectHashSeed;");
    for (const std::string& dep : effect.deps) {
        std::string member;
        std::string type;
        if (!dep_member(component, dep, member, type, error)) {
            return false;
        }
        writer.line("hash = tessera::ui::effect_hash(hash, component." + member + ");");
    }
    writer.line("return hash;");
    --writer.indent;
    writer.line("}");
    return true;
}

} // namespace

bool emit_lifecycle(const ir::Component& component, const std::vector<const ir::Binding*>& bindings,
                    CppWriter& writer, std::string& error) {
    if (component.effects.empty()) {
        writer.line("");
        writer.line("void " + component.name + "::mount() {}");
        writer.line("");
        writer.line("void " + component.name + "::update_effects() {}");
        writer.line("");
        writer.line("void " + component.name + "::unmount() {}");
        writer.line("");
        writer.open("void " + component.name + "::update()");
        writer.line("update_effects();");
        writer.line("apply_state();");
        writer.line("state_dirty_ = false;");
        writer.close("");
        writer.line("");
        return true;
    }

    writer.line("namespace {");
    ++writer.indent;
    for (std::size_t i = 0; i < component.effects.size(); ++i) {
        if (!emit_dep_hash(writer, component, i, component.effects[i], error)) {
            return false;
        }
        writer.line("");
    }
    --writer.indent;
    writer.line("} // namespace");
    writer.line("");

    writer.open("void " + component.name + "::mount()");
    writer.line("update_effects();");
    writer.close("");
    writer.line("");

    writer.open("void " + component.name + "::update_effects()");
    for (std::size_t i = 0; i < component.effects.size(); ++i) {
        const ir::Effect& effect = component.effects[i];
        const std::string state = effect_state_name(effect.id);
        const ir::Binding* handler = find_binding(bindings, effect.handler);
        if (handler == nullptr) {
            return fail(error, "effect `" + effect.id + "` names unknown handler `" +
                                   effect.handler + "`");
        }
        const std::string hash = "dep_hash_" + std::to_string(i) + "(*this)";
        writer.line("// " + effect.id + " (" + effect.handler +
                    (effect.cleanup.empty() ? "" : ", " + effect.cleanup) + ")");
        if (!effect.cleanup.empty()) {
            const ir::Binding* cleanup = find_binding(bindings, effect.cleanup);
            if (cleanup == nullptr) {
                return fail(error, "effect `" + effect.id + "` names unknown cleanup `" +
                                       effect.cleanup + "`");
            }
            writer.line("if (" + state + ".active && " + state + ".deps_hash != " + hash + ") {");
            ++writer.indent;
            writer.line(cleanup->region + "();");
            writer.line("tessera::ui::effect_mark_cleaned(" + state + ");");
            --writer.indent;
            writer.line("}");
        }
        writer.line("if (tessera::ui::effect_needs_run(" + state + ", " + hash + ")) {");
        ++writer.indent;
        writer.line(handler->region + "();");
        --writer.indent;
        writer.line("}");
    }
    writer.close("");
    writer.line("");

    writer.open("void " + component.name + "::update()");
    writer.line("update_effects();");
    writer.line("apply_state();");
    writer.line("state_dirty_ = false;");
    writer.close("");
    writer.line("");

    writer.open("void " + component.name + "::unmount()");
    for (std::size_t i = component.effects.size(); i-- > 0;) {
        const ir::Effect& effect = component.effects[i];
        const std::string state = effect_state_name(effect.id);
        if (effect.cleanup.empty()) {
            writer.line("tessera::ui::effect_mark_cleaned(" + state + ");");
            continue;
        }
        const ir::Binding* cleanup = find_binding(bindings, effect.cleanup);
        if (cleanup == nullptr) {
            return fail(error, "effect `" + effect.id + "` names unknown cleanup `" +
                                   effect.cleanup + "`");
        }
        writer.line("if (" + state + ".active) {");
        ++writer.indent;
        writer.line(cleanup->region + "();");
        writer.line("tessera::ui::effect_mark_cleaned(" + state + ");");
        --writer.indent;
        writer.line("}");
    }
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
