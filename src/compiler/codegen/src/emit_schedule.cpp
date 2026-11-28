// SPDX-License-Identifier: MIT

#include "emit_dirty.hpp"
#include "emit_dynamic.hpp"
#include "emit_events.hpp"
#include "emit_schedule.hpp"
#include "emit_types_header.hpp"

#include <cctype>
#include <optional>
#include <string_view>

namespace tessera::compiler::codegen::internal {
namespace {

// A setter is named after the source state. A name that is not a plain C++ identifier would not
// compile, so the setter is skipped (the field stays reachable by direct assignment plus
// `schedule_update()`).
bool plain_identifier(std::string_view name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
        return false;
    }
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
            return false;
        }
    }
    return true;
}

std::string setter_name(const ir::State& state) {
    return "set_" + state.name;
}

// True when the state's declared type is exactly a declared record (`Form`, not `list<Form>` or
// `optional<Form>`). Only then does the cell split into per-field bits and does its setter compare
// field by field (ADR-051 implementation note #834). A wrapper-typed cell is one bit and compares
// whole, like any other value.
bool is_record_state(const ir::State& state, const ir::IrModule& module) {
    return find_record_type(module, state.type) != nullptr;
}

// Runs the handler recorded by the set that just completed (`onComplete`), once. The member is
// cleared before the call so a handler that sets the same value again records its own callback.
void emit_completion_dispatch(CppWriter& writer, const ir::Component& component,
                              const ir::AnimatedValue& value) {
    if (!animated_has_completion(component, value.id)) {
        return;
    }
    const std::string controller = animated_controller_name(value.id);
    const std::string member = animated_on_complete_name(value.id);
    writer.open("if (" + controller + ".take_completed())");
    writer.line("const int on_complete = " + member + ";");
    writer.line(member + " = 0;");
    writer.open("switch (on_complete)");
    for (std::size_t i = 0; i < component.handlers.size(); ++i) {
        const ir::Handler& handler = component.handlers[i];
        bool used = false;
        for (const ir::Handler& source : component.handlers) {
            for (const ir::Action& action : source.actions) {
                used = used || (action.kind == ir::ActionKind::AnimSet &&
                                action.target == value.id && action.on_complete == handler.id);
            }
        }
        if (used) {
            writer.line("case " + std::to_string(i + 1) + ": " + method_name(handler.id) +
                        "(tessera::ui::Event{}); break;");
        }
    }
    writer.line("default: break;");
    writer.close("");
    writer.close("");
}

} // namespace

bool emit_schedule_declarations(CppWriter& writer, const ir::IrModule& module,
                                const ir::Component& component, std::string& error) {
    writer.line("// Scheduled updates: a setter marks this component dirty and the host's");
    writer.line(
        "// UpdateScheduler runs update() once at its next flush (UI thread only). The ticket");
    writer.line("// address must stay stable while attached.");
    writer.line("tessera::ui::UpdateTicket update_ticket_;");
    writer.line("void attach_scheduler(tessera::ui::UpdateScheduler& scheduler);");
    writer.line("void detach_scheduler(tessera::ui::UpdateScheduler& scheduler);");
    writer.line("void schedule_update();");
    if (!component.animated_values.empty()) {
        writer.line(
            "// Advances the animated values by dt seconds; true while any is still moving.");
        writer.line("bool tick_animations(double dt);");
        writer.line(
            "// Reduced motion: every animated value snaps to its target and stays snapped.");
        writer.line("void apply_reduced_motion(bool reduced);");
    }
    for (const ir::State& state : component.state) {
        if (!plain_identifier(state.name)) {
            continue;
        }
        const std::optional<std::string> type = cpp_type(state.type, module.types);
        if (!type) {
            error = "state `" + state.name + "` has a type with no C++ form (`" + state.type + "`)";
            return false;
        }
        writer.line("void " + setter_name(state) + "(" + *type + " value);");
    }
    return true;
}

bool emit_schedule_definitions(CppWriter& writer, const ir::IrModule& module,
                               const ir::Component& component,
                               const std::vector<ComponentInstance>& instances,
                               const DynamicPlan& plan, std::string& error) {
    const bool item_instances = plan_has_item_instances(plan);
    writer.open("void " + component.name +
                "::attach_scheduler(tessera::ui::UpdateScheduler& scheduler)");
    writer.line("update_ticket_.context = this;");
    writer.line("update_ticket_.update = [](void* context) {");
    ++writer.indent;
    writer.line("static_cast<" + component.name + "*>(context)->flush_update();");
    --writer.indent;
    writer.line("};");
    if (!component.animated_values.empty()) {
        writer.line("update_ticket_.tick = [](void* context, double dt) {");
        ++writer.indent;
        writer.line("return static_cast<" + component.name + "*>(context)->tick_animations(dt);");
        --writer.indent;
        writer.line("};");
        writer.line("update_ticket_.motion = [](void* context, bool reduced) {");
        ++writer.indent;
        writer.line("static_cast<" + component.name +
                    "*>(context)->apply_reduced_motion(reduced);");
        --writer.indent;
        writer.line("};");
    }
    writer.line("scheduler.attach(update_ticket_);");
    for (const ComponentInstance& instance : instances) {
        if (instance.in_template()) {
            continue; // the item record owns it; the item path attaches it below
        }
        writer.line(instance_member(instance.ordinal) + ".attach_scheduler(scheduler);");
    }
    if (item_instances) {
        // Live item instances join here, and a late-created item joins through the stored pointer
        // (docs/design/compiler-node-state.md section 3).
        writer.line("scheduler_ = &scheduler;");
        emit_item_instance_calls(writer, plan, "attach_scheduler", "scheduler");
    }
    writer.close("");
    writer.line("");

    writer.open("void " + component.name +
                "::detach_scheduler(tessera::ui::UpdateScheduler& scheduler)");
    for (std::size_t i = instances.size(); i-- > 0;) {
        if (instances[i].in_template()) {
            continue;
        }
        writer.line(instance_member(instances[i].ordinal) + ".detach_scheduler(scheduler);");
    }
    if (item_instances) {
        emit_item_instance_calls(writer, plan, "detach_scheduler", "scheduler", true);
        writer.line("scheduler_ = nullptr;");
    }
    writer.line("scheduler.detach(update_ticket_);");
    writer.close("");
    writer.line("");

    writer.open("void " + component.name + "::schedule_update()");
    writer.line("update_ticket_.schedule();");
    writer.close("");
    writer.line("");

    if (!component.animated_values.empty()) {
        writer.open("bool " + component.name + "::tick_animations(double dt)");
        writer.line("bool moving = false;");
        writer.line("bool changed = false;");
        for (const ir::AnimatedValue& value : component.animated_values) {
            const std::string controller = animated_controller_name(value.id);
            writer.line("moving = " + controller + ".tick(dt) || moving;");
            writer.open("if (" + member_name(value.id) + " != " + controller + ".value())");
            writer.line(member_name(value.id) + " = " + controller + ".value();");
            writer.line("dirty_.set(" + dirty_bit_name(value.id) + ");");
            writer.line("changed = true;");
            writer.close("");
            emit_completion_dispatch(writer, component, value);
        }
        writer.open("if (changed)");
        writer.line("schedule_update();");
        writer.close("");
        writer.line("return moving;");
        writer.close("");
        writer.line("");

        writer.open("void " + component.name + "::apply_reduced_motion(bool reduced)");
        writer.line("bool changed = false;");
        for (const ir::AnimatedValue& value : component.animated_values) {
            const std::string controller = animated_controller_name(value.id);
            writer.line(controller + ".set_reduced_motion(reduced);");
            // A tween finished by reduced motion still completes: the next tick delivers it.
            writer.open("if (" + member_name(value.id) + " != " + controller + ".value() || " +
                        controller + ".completion_pending())");
            writer.line(member_name(value.id) + " = " + controller + ".value();");
            writer.line("dirty_.set(" + dirty_bit_name(value.id) + ");");
            writer.line("changed = true;");
            writer.close("");
        }
        writer.open("if (changed)");
        writer.line("schedule_update();");
        writer.close("");
        writer.close("");
        writer.line("");
    }

    for (const ir::State& state : component.state) {
        if (!plain_identifier(state.name)) {
            continue;
        }
        const std::optional<std::string> type = cpp_type(state.type, module.types);
        if (!type) {
            error = "state `" + state.name + "` has a type with no C++ form (`" + state.type + "`)";
            return false;
        }
        writer.open("void " + component.name + "::" + setter_name(state) + "(" + *type + " value)");
        if (state.initial && reads_prop(*state.initial)) {
            // A value set before build() is the caller's choice and must not be re-seeded from props.
            writer.line(state_seeded_name(state.id) + " = true;");
        }
        // Equality-gated (ADR-041, decision 6): an unchanged value schedules nothing, so a repeated
        // identical set neither wakes the loop nor runs update(). NaN never compares equal and
        // therefore always schedules, which is the safe direction. A record now has generated
        // equality (ADR-051 implementation note #834), so its whole-record setter is gated too.
        if (!is_record_state(state, module)) {
            writer.open("if (" + member_name(state.id) + " == value)");
            writer.line("return;");
            writer.close("");
            writer.line(member_name(state.id) + " = std::move(value);");
            writer.line("dirty_.set(" + dirty_bit_name(state.id) + ");");
            writer.line("schedule_update();");
            writer.close("");
            writer.line("");
            continue;
        }
        // A record cell splits into one bit per read leaf path (ADR-056 decision 1, #834), so a
        // whole-record set compares field by field and raises only the bits of the fields that
        // changed. `note_dirty` marks a bit without scheduling; one `schedule_update()` follows
        // only when some bit was raised, so an equal record still schedules nothing.
        const ir::RecordType* record = find_record_type(module, state.type);
        const std::string mask = member_name(state.id);
        writer.line("bool tessera_changed = false;");
        for (const ir::RecordMember& member : record->members) {
            const std::string field = mask + "." + member.name;
            writer.open("if (!(" + field + " == value." + member.name + "))");
            writer.line("tessera_changed = true;");
            for (const std::string& bit : cell_bit_ids(state.id, record->name, module.types)) {
                if (!bit.starts_with(std::string(state.id) + "." + member.name)) {
                    continue;
                }
                writer.line("note_dirty(" + dirty_bit_name(bit) + ");");
            }
            writer.close("");
        }
        writer.line(mask + " = std::move(value);");
        writer.open("if (tessera_changed)");
        writer.line("schedule_update();");
        writer.close("");
        writer.close("");
        writer.line("");
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
