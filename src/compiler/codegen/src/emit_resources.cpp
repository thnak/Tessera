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

// The C++ key-writer calls for one resource's key, or false with `error`.
bool emit_key_fields(CppWriter& writer, const ir::Component& component,
                     const ir::Resource& resource, std::string& error) {
    const auto write_value = [&](const std::string& name, const ir::Expr& expr) {
        const ExprEmission value = emit_expr(expr, component);
        if (!value.error.empty()) {
            error = value.error;
            return false;
        }
        writer.line("key.field(\"" + name + "\", " + value.text + ");");
        return true;
    };
    if (resource.key.kind != ir::ExprKind::Record) {
        return write_value("", resource.key);
    }
    for (const ir::RecordField& field : resource.key.fields) {
        if (!field.value || !write_value(field.name, *field.value)) {
            if (error.empty()) {
                error =
                    "resource `" + resource.id + "` key field `" + field.name + "` has no value";
            }
            return false;
        }
    }
    return true;
}

} // namespace

const ir::Service* resource_service(const ir::Component& component, const ir::Resource& resource) {
    for (const ir::Service& service : component.services) {
        if (service.id == resource.service) {
            return &service;
        }
    }
    return nullptr;
}

bool emit_key_sync(CppWriter& writer, const ir::Component& component, std::string& error) {
    writer.open("bool " + component.name + "::sync_resource_keys()");
    writer.line("bool rebound = false;");
    for (const ir::Resource& resource : component.resources) {
        const ir::Service* service = resource_service(component, resource);
        if (service == nullptr) {
            return fail(error, "resource `" + resource.id + "` names undeclared service `" +
                                   resource.service + "`");
        }
        const std::string stem = resource_stem(resource);
        writer.line("// `" + resource.id + "`: bound through the service when it is a");
        writer.line("// `ResourceProvider`, by the host otherwise.");
        writer.line("if constexpr (tessera::ui::pending::ResourceProvider<" + service->type +
                    ">) {");
        ++writer.indent;
        writer.line(service->type + "* const provider = std::get<" + service->type +
                    "*>(services_);");
        writer.line("if (!" + stem + "_manual_ && provider != nullptr) {");
        ++writer.indent;
        writer.line("tessera::ui::pending::ResourceKey key;");
        if (!emit_key_fields(writer, component, resource, error)) {
            return false;
        }
        writer.line("if (!key.overflow() && !(" + stem + "_keyed_ && key.same_as(" + stem +
                    "_key_))) {");
        ++writer.indent;
        writer.line(stem + "_key_ = key;");
        writer.line(stem + "_keyed_ = true;");
        writer.line(resource_view_member(resource) +
                    " = tessera::ui::pending::provided_view(*provider, \"" + resource.method +
                    "\", key.view());");
        writer.line("rebound = true;");
        --writer.indent;
        writer.line("}");
        --writer.indent;
        writer.line("}");
        --writer.indent;
        writer.line("}");
    }
    writer.line("return rebound;");
    writer.close("");
    writer.line("");
    return true;
}

void emit_resource_sync_line(CppWriter& writer, const ir::Component& component) {
    if (!uses_resources(component)) {
        return;
    }
    // A key may have changed since the last frame (a plain setter only schedules), and an effect
    // that depends on a resource must see the state this frame publishes.
    writer.line("(void)sync_resource_keys();");
    writer.line("(void)pending_sync_run();");
}

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

void emit_pending_dirty_mark(CppWriter& writer, const std::vector<std::string>& terms,
                             bool returns_changed) {
    // pending-ui-v1.md section 10.1: when what the model publishes changed since the last sync,
    // mark the pending cell (every binding that reads `res::`, `isPending` or a deferred value
    // carries it) and schedule the update, so the host need not call update() after a sync.
    writer.line("std::uint64_t pending_word = 1469598103934665603ull;");
    for (const std::string& term : terms) {
        writer.line("pending_word = tessera::ui::pending::mix_signature(pending_word, " + term +
                    ");");
    }
    if (returns_changed) {
        writer.line("const bool pending_changed = pending_word != pending_sig_;");
        writer.line("if (pending_changed) {");
    } else {
        writer.line("if (pending_word != pending_sig_) {");
    }
    ++writer.indent;
    writer.line("pending_sig_ = pending_word;");
    writer.line("dirty_.set(" + dirty_bit_name(std::string(kPendingCell)) + ");");
    if (!returns_changed) {
        writer.line("schedule_update();");
    }
    --writer.indent;
    writer.line("}");
}

void emit_resource_fields(CppWriter& writer, const ir::Component& component) {
    for (const ir::Resource& resource : component.resources) {
        writer.line("// `" + resource.id + "` (" + resource.service + "." + resource.method +
                    (resource.suspend ? ", suspending" : "") +
                    "): bound by the host; unbound reads as idle.");
        writer.line("tessera::ui::pending::ResourceView " + resource_view_member(resource) + ";");
        writer.line(
            "// The key last bound through a `ResourceProvider` service, and whether the host");
        writer.line("// bound the view by hand (then the provider is not asked).");
        writer.line("tessera::ui::pending::ResourceKey " + resource_stem(resource) + "_key_;");
        writer.line("bool " + resource_stem(resource) + "_keyed_ = false;");
        writer.line("bool " + resource_stem(resource) + "_manual_ = false;");
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
    writer.line(
        "// Binds each resource whose service is a `ResourceProvider` (pending-ui-v1.md section");
    writer.line(
        "// 10.2) to the view of its current key; true when any binding changed. A resource");
    writer.line("// the host bound by hand is left alone.");
    writer.line("bool sync_resource_keys();");
    writer.line("// Reads every bound resource once, feeds the boundary reads and the transition");
    writer.line("// mark, re-applies the boundaries and marks the pending cell when anything the");
    writer.line("// bindings read changed. True when it did. pending_sync() also binds keys and");
    writer.line("// schedules the update: what a subscription does at a frame boundary.");
    writer.line("bool pending_sync_run();");
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
        writer.line(stem + "_manual_ = true;");
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

    // One read per resource per sync: every boundary slot, the transition mark and the dirty
    // signature see one state. A resource no boundary covers is still read, because a binding may
    // show its status or data.
    std::set<std::size_t> used;
    const ir::SuspenseReads reads = ir::collect_suspense_reads(component);
    for (std::size_t index = 0; index < component.resources.size(); ++index) {
        used.insert(index);
    }
    writer.open("bool " + owner + "pending_sync_run()");
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
    std::vector<std::string> terms;
    for (const std::size_t index : used) {
        terms.push_back(resource_stem(component.resources[index]) + "_now.signature()");
    }
    if (transition) {
        terms.emplace_back("pending_transition_.pending() ? 1u : 0u");
    }
    for (const SuspenseSite& site : plan.suspenses) {
        terms.push_back(boundary_name(site.ordinal) + ".fallback_live() ? 1u : 0u");
    }
    emit_pending_dirty_mark(writer, terms, true);
    for (const SuspenseSite& site : plan.suspenses) {
        writer.line("apply_suspense_" + std::to_string(site.ordinal) + "();");
    }
    writer.line("return pending_changed;");
    writer.close("");
    writer.line("");
    writer.open("void " + owner + "pending_sync()");
    writer.line("(void)sync_resource_keys();");
    writer.line("if (pending_sync_run()) {");
    ++writer.indent;
    writer.line("schedule_update();");
    --writer.indent;
    writer.line("}");
    writer.close("");
    writer.line("");
    if (!emit_key_sync(writer, component, error)) {
        return false;
    }
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
