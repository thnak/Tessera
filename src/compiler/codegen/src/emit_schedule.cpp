// SPDX-License-Identifier: MIT

#include "emit_schedule.hpp"

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

} // namespace

bool emit_schedule_declarations(CppWriter& writer, const ir::Component& component,
                                std::string& error) {
    writer.line("// Scheduled updates: a setter marks this component dirty and the host's");
    writer.line(
        "// UpdateScheduler runs update() once at its next flush (UI thread only). The ticket");
    writer.line("// address must stay stable while attached.");
    writer.line("tessera::ui::UpdateTicket update_ticket_;");
    writer.line("void attach_scheduler(tessera::ui::UpdateScheduler& scheduler);");
    writer.line("void detach_scheduler(tessera::ui::UpdateScheduler& scheduler);");
    writer.line("void schedule_update();");
    for (const ir::State& state : component.state) {
        if (!plain_identifier(state.name)) {
            continue;
        }
        const std::optional<std::string> type = cpp_type(state.type);
        if (!type) {
            error = "state `" + state.name + "` has a type with no C++ form (`" + state.type + "`)";
            return false;
        }
        writer.line("void " + setter_name(state) + "(" + *type + " value);");
    }
    return true;
}

bool emit_schedule_definitions(CppWriter& writer, const ir::Component& component,
                               const std::vector<ComponentInstance>& instances,
                               std::string& error) {
    writer.open("void " + component.name +
                "::attach_scheduler(tessera::ui::UpdateScheduler& scheduler)");
    writer.line("update_ticket_.context = this;");
    writer.line("update_ticket_.update = [](void* context) {");
    ++writer.indent;
    writer.line("static_cast<" + component.name + "*>(context)->update();");
    --writer.indent;
    writer.line("};");
    writer.line("scheduler.attach(update_ticket_);");
    for (const ComponentInstance& instance : instances) {
        writer.line(instance_member(instance.ordinal) + ".attach_scheduler(scheduler);");
    }
    writer.close("");
    writer.line("");

    writer.open("void " + component.name +
                "::detach_scheduler(tessera::ui::UpdateScheduler& scheduler)");
    for (std::size_t i = instances.size(); i-- > 0;) {
        writer.line(instance_member(instances[i].ordinal) + ".detach_scheduler(scheduler);");
    }
    writer.line("scheduler.detach(update_ticket_);");
    writer.close("");
    writer.line("");

    writer.open("void " + component.name + "::schedule_update()");
    writer.line("update_ticket_.schedule();");
    writer.close("");
    writer.line("");

    for (const ir::State& state : component.state) {
        if (!plain_identifier(state.name)) {
            continue;
        }
        const std::optional<std::string> type = cpp_type(state.type);
        if (!type) {
            error = "state `" + state.name + "` has a type with no C++ form (`" + state.type + "`)";
            return false;
        }
        writer.open("void " + component.name + "::" + setter_name(state) + "(" + *type + " value)");
        // Equality-gated (ADR-041, decision 6): an unchanged value schedules nothing, so a repeated
        // identical set neither wakes the loop nor runs update(). NaN never compares equal and
        // therefore always schedules, which is the safe direction.
        writer.open("if (" + member_name(state.id) + " == value)");
        writer.line("return;");
        writer.close("");
        writer.line(member_name(state.id) + " = std::move(value);");
        writer.line("schedule_update();");
        writer.close("");
        writer.line("");
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
