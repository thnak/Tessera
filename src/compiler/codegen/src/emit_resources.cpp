// SPDX-License-Identifier: MIT

// The generated resource views, boundary reads and transition wiring (#656).

#include "emit_resources.hpp"
#include "emit_suspense.hpp"
#include "tessera/compiler/ir/suspense_reads.hpp"

#include <algorithm>
#include <set>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::string slot_text(std::size_t slot) {
    return std::to_string(slot);
}

std::string boundary_name(unsigned ordinal) {
    return "suspense_" + std::to_string(ordinal) + "_";
}

// The resource the component's marked setters wait for, or npos when it has none.
bool marked_resource(const ir::Component& component, std::size_t& index, std::string& error) {
    index = static_cast<std::size_t>(-1);
    for (const ir::Handler& handler : component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (!action.transition) {
                continue;
            }
            std::size_t found = 0;
            if (!transition_resource(component, action.target, found, error)) {
                return false;
            }
            if (index != static_cast<std::size_t>(-1) && index != found) {
                return fail(error, "transition-marked setters wait for different resources; the "
                                   "mark holds one epoch");
            }
            index = found;
        }
    }
    return true;
}

} // namespace

bool uses_resources(const ir::Component& component) {
    return !component.resources.empty();
}

std::string resource_stem(const ir::Resource& resource) {
    std::string stem = member_name(resource.id); // `res_feed_`
    if (!stem.empty() && stem.back() == '_') {
        stem.pop_back();
    }
    return stem;
}

std::string resource_view_member(const ir::Resource& resource) {
    return resource_stem(resource) + "_view_";
}

std::vector<std::size_t> boundary_reads(const ir::Component& component, const ir::Node& boundary) {
    const ir::SuspenseReads reads = ir::collect_suspense_reads(component);
    for (const ir::BoundaryReads& candidate : reads.boundaries) {
        if (candidate.boundary == &boundary) {
            return candidate.resources;
        }
    }
    return {};
}

bool transition_resource(const ir::Component& component, std::string_view target,
                         std::size_t& index, std::string& error) {
    if (const std::optional<std::size_t> found = ir::resource_keyed_by(component, target)) {
        index = *found;
        return true;
    }
    return fail(error, "the transition-marked set of `" + std::string(target) +
                           "` writes no resource key, so there is nothing for the transition to "
                           "wait for");
}

void emit_resource_fields(CppWriter& writer, const ir::Component& component) {
    for (const ir::Resource& resource : component.resources) {
        writer.line("// `" + resource.id + "` (" + resource.service + "." + resource.method +
                    (resource.suspend ? ", suspending" : "") +
                    "): bound by the host; unbound reads as idle.");
        writer.line("tessera::ui::pending::ResourceView " + resource_view_member(resource) + ";");
    }
}

void emit_resource_declarations(CppWriter& writer, const ir::Component& component) {
    writer.line(
        "// The resource seams (#656): the host binds a view per resource (a fake resource or");
    writer.line(
        "// the data store), and pending_sync() publishes their state to every boundary read.");
    for (const ir::Resource& resource : component.resources) {
        const std::string stem = resource_stem(resource);
        writer.line("void bind_" + stem + "(const tessera::ui::pending::ResourceView& view);");
        writer.line("[[nodiscard]] tessera::ui::pending::ResourceState " + stem +
                    "_state() const noexcept;");
        writer.line("[[nodiscard]] const char* " + stem + "_status() const noexcept;");
    }
    writer.line("// Reads every bound resource once, feeds the boundary reads and the transition");
    writer.line(
        "// mark, and re-applies the boundaries: what a subscription does at a frame boundary.");
    writer.line("void pending_sync();");
    writer.line("[[nodiscard]] bool pending_is_pending() const;");
    writer.line("[[nodiscard]] bool pending_fallback_live() const;");
    writer.line("// The marked setter, as the Button's click binding runs it.");
    writer.line("void run_marked_set();");
}

bool emit_resource_sources(CppWriter& writer, const ir::Component& component,
                           const DynamicPlan& plan, std::string& error) {
    std::size_t marked = 0;
    if (!marked_resource(component, marked, error)) {
        return false;
    }
    const bool transition = marked != static_cast<std::size_t>(-1);
    const std::string owner = component.name + "::";
    for (const ir::Resource& resource : component.resources) {
        const std::string stem = resource_stem(resource);
        writer.open("void " + owner + "bind_" + stem +
                    "(const tessera::ui::pending::ResourceView& view)");
        writer.line(resource_view_member(resource) + " = view;");
        writer.line("pending_sync();");
        writer.close("");
        writer.line("");
        writer.open("tessera::ui::pending::ResourceState " + owner + stem +
                    "_state() const noexcept");
        writer.line("return " + resource_view_member(resource) + ".read();");
        writer.close("");
        writer.line("");
        writer.open("const char* " + owner + stem + "_status() const noexcept");
        writer.line("return " + resource_view_member(resource) + ".read().status();");
        writer.close("");
        writer.line("");
    }

    // One read per resource per sync: every boundary slot and the transition mark see one state.
    std::set<std::size_t> used;
    const ir::SuspenseReads reads = ir::collect_suspense_reads(component);
    for (const ir::BoundaryReads& boundary : reads.boundaries) {
        used.insert(boundary.resources.begin(), boundary.resources.end());
    }
    if (transition) {
        used.insert(marked);
    }
    writer.open("void " + owner + "pending_sync()");
    for (const std::size_t index : used) {
        writer.line("const tessera::ui::pending::ResourceState " +
                    resource_stem(component.resources[index]) +
                    "_now = " + resource_view_member(component.resources[index]) + ".read();");
    }
    // The mark observes first, so a boundary above the transition's read holds its content only
    // while the transition is pending (pending-ui-v1.md section 4.3).
    if (transition) {
        const std::string now = resource_stem(component.resources[marked]) + "_now";
        writer.line("if (pending_transition_.started()) {");
        ++writer.indent;
        writer.line("(void)pending_transition_.observe(" + now + ".loading, " + now + ".epoch);");
        --writer.indent;
        writer.line("}");
    }
    for (const ir::BoundaryReads& boundary : reads.boundaries) {
        // Boundaries are numbered by the plan in the same walk order as `collect_suspense_reads`.
        const std::size_t position = static_cast<std::size_t>(&boundary - reads.boundaries.data());
        if (position >= plan.suspenses.size()) {
            return fail(error, "the Suspense boundary `" + boundary.boundary->id +
                                   "` is missing from the dynamic plan");
        }
        const unsigned ordinal = plan.suspenses[position].ordinal;
        if (transition && std::find(boundary.resources.begin(), boundary.resources.end(), marked) !=
                              boundary.resources.end()) {
            writer.line(boundary_name(ordinal) + ".set_hold(pending_transition_.pending());");
        }
        for (std::size_t slot = 0; slot < boundary.resources.size(); ++slot) {
            const std::string now =
                resource_stem(component.resources[boundary.resources[slot]]) + "_now";
            writer.line("(void)" + boundary_name(ordinal) + ".set_read(" + slot_text(slot) + ", " +
                        now + ".loading, " + now + ".has_data, " + now + ".epoch);");
        }
    }
    for (const SuspenseSite& site : plan.suspenses) {
        writer.line("apply_suspense_" + std::to_string(site.ordinal) + "();");
    }
    writer.close("");
    writer.line("");
    writer.open("bool " + owner + "pending_is_pending() const");
    writer.line(transition ? "return pending_transition_.pending();" : "return false;");
    writer.close("");
    writer.line("");
    writer.open("bool " + owner + "pending_fallback_live() const");
    if (plan.suspenses.empty()) {
        writer.line("return false;");
    } else {
        writer.line("return " + boundary_name(plan.suspenses.front().ordinal) +
                    ".fallback_live();");
    }
    writer.close("");
    writer.line("");
    return emit_run_marked_set(writer, component, false, error);
}

} // namespace tessera::compiler::codegen::internal
