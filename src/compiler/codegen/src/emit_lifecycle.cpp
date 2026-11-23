// SPDX-License-Identifier: MIT

// Component lifecycle emission (docs/design/compiler.md section 11): `useEffect` deps-hash helpers
// plus mount/update_effects/unmount, and the ref-target assignment the generated build() performs.

#include "codegen_internal.hpp"
#include "emit_dynamic.hpp"
#include "emit_events.hpp"
#include "emit_types_header.hpp"

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

// Resolves one effect dependency to its generated member and C++ type. A record-typed cell is
// allowed (its type now has a generated `hash_value`, ADR-051 implementation note #834); the
// caller picks the hash spelling from `is_record`.
bool dep_member(const ir::Component& component, const ir::IrModule& module, std::string_view dep,
                std::string& member, std::string& type, bool& is_record, std::string& error) {
    const auto resolve_record = [&](std::string_view ir_type) {
        return find_record_type(module, ir_type) != nullptr;
    };
    for (const ir::State& state : component.state) {
        if (state.id == dep) {
            const std::optional<std::string> mapped = cpp_type(state.type, module.types);
            if (!mapped) {
                return fail(error,
                            "effect dep `" + std::string(dep) + "` has a type with no C++ form");
            }
            member = member_name(state.id);
            type = *mapped;
            is_record = resolve_record(state.type);
            return true;
        }
    }
    for (const ir::Prop& prop : component.props) {
        if ("prop::" + prop.name != dep) {
            continue;
        }
        const std::optional<std::string> mapped = cpp_type(prop.type, module.types);
        if (!mapped) {
            return fail(error, "effect dep `" + std::string(dep) + "` has a type with no C++ form");
        }
        member = member_name("prop::" + prop.name);
        type = *mapped;
        is_record = resolve_record(prop.type);
        return true;
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (value.id == dep) {
            const std::optional<std::string> mapped = cpp_type(value.type, module.types);
            if (!mapped) {
                return fail(error,
                            "effect dep `" + std::string(dep) + "` has a type with no C++ form");
            }
            member = member_name(value.id);
            type = *mapped;
            is_record = resolve_record(value.type);
            return true;
        }
    }
    return fail(error, "effect dep `" + std::string(dep) +
                           "` is not a state, prop or animated value reference");
}

bool emit_dep_hash(CppWriter& writer, const ir::Component& component, const ir::IrModule& module,
                   std::size_t index, const ir::Effect& effect, std::string& error) {
    writer.line("std::uint64_t dep_hash_" + std::to_string(index) + "(const " + component.name +
                "& component) {");
    ++writer.indent;
    writer.line("std::uint64_t hash = tessera::ui::kEffectHashSeed;");
    for (const std::string& dep : effect.deps) {
        std::string member;
        std::string type;
        bool is_record = false;
        if (!dep_member(component, module, dep, member, type, is_record, error)) {
            return false;
        }
        // A record folds its generated field-wise hash in as a 64-bit value; every other type uses
        // the scalar/list overload directly.
        writer.line("hash = tessera::ui::effect_hash(hash, " +
                    (is_record ? "hash_value(component." + member + ")" : "component." + member) +
                    ");");
    }
    writer.line("return hash;");
    --writer.indent;
    writer.line("}");
    return true;
}

} // namespace

bool emit_lifecycle(const ir::Component& component, const ir::IrModule& module,
                    const std::vector<const ir::Binding*>& bindings,
                    const std::vector<ComponentInstance>& instances, const DynamicPlan& plan,
                    CppWriter& writer, std::string& error) {
    const bool item_instances = plan_has_item_instances(plan);
    const auto static_instances = [&]() {
        std::vector<const ComponentInstance*> result;
        for (const ComponentInstance& instance : instances) {
            if (!instance.in_template()) {
                result.push_back(&instance);
            }
        }
        return result;
    }();
    const auto instance_calls = [&](std::string_view method, bool reverse) {
        if (reverse) {
            for (std::size_t i = static_instances.size(); i-- > 0;) {
                writer.line(instance_member(static_instances[i]->ordinal) + "." +
                            std::string(method) + "();");
            }
            return;
        }
        for (const ComponentInstance* instance : static_instances) {
            writer.line(instance_member(instance->ordinal) + "." + std::string(method) + "();");
        }
    };
    // Live per-item instances mount/unmount/update with their owner; `mounted_` is the owner's flag
    // so an item created later mounts immediately (docs/design/compiler-node-state.md section 3).
    const auto item_calls = [&](std::string_view method, bool reverse) {
        if (item_instances) {
            emit_item_instance_calls(writer, plan, method, {}, reverse);
        }
    };
    // A hand call of update() evaluates every binding (ADR-056 decision 7); the scheduler runs
    // flush_update(), which evaluates only the dirty ones.
    const auto hand_update = [&]() {
        writer.open("void " + component.name + "::update()");
        writer.line("mark_all_dirty();");
        writer.line("flush_update();");
        writer.close("");
        writer.line("");
    };
    const auto mount_items = [&]() {
        if (!item_instances) {
            return;
        }
        item_calls("mount", false);
        writer.line("mounted_ = true;");
    };
    const auto unmount_items = [&]() {
        if (!item_instances) {
            return;
        }
        writer.line("mounted_ = false;");
        item_calls("unmount", true);
    };
    if (component.effects.empty()) {
        writer.line("");
        if (static_instances.empty() && !item_instances) {
            writer.line("void " + component.name + "::mount() {}");
            writer.line("");
            writer.line("void " + component.name + "::update_effects() {}");
            writer.line("");
            writer.line("void " + component.name + "::unmount() {}");
            writer.line("");
            hand_update();
            writer.open("void " + component.name + "::flush_update()");
            writer.line("update_effects();");
            writer.line("apply_state();");
            writer.close("");
            writer.line("");
            return true;
        }
        writer.open("void " + component.name + "::mount()");
        instance_calls("mount", false);
        mount_items();
        writer.close("");
        writer.line("");
        writer.open("void " + component.name + "::update_effects()");
        writer.close("");
        writer.line("");
        writer.open("void " + component.name + "::unmount()");
        unmount_items();
        instance_calls("unmount", true);
        writer.close("");
        writer.line("");
        hand_update();
        writer.open("void " + component.name + "::flush_update()");
        writer.line("update_effects();");
        writer.line("apply_state();");
        writer.close("");
        writer.line("");
        return true;
    }

    writer.line("namespace {");
    ++writer.indent;
    for (std::size_t i = 0; i < component.effects.size(); ++i) {
        if (!emit_dep_hash(writer, component, module, i, component.effects[i], error)) {
            return false;
        }
        writer.line("");
    }
    --writer.indent;
    writer.line("} // namespace");
    writer.line("");

    writer.open("void " + component.name + "::mount()");
    writer.line("update_effects();");
    instance_calls("mount", false);
    mount_items();
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
    writer.line("apply_state();");
    writer.close("");
    writer.line("");

    hand_update();
    writer.open("void " + component.name + "::flush_update()");
    writer.line("update_effects();");
    writer.line("apply_state();");
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
    unmount_items();
    instance_calls("unmount", true);
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
