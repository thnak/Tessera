// SPDX-License-Identifier: MIT

// ADR-067 spike #661, off by default: the generated half of the pending UI.
//
// A `Suspense` boundary is a static node (ADR-067 item 2): it owns no scene node and no layout box.
// The generated component owns both subtrees in ownership slots and attaches whichever one the
// boundary's model says is live, at the source position the boundary occupied in the tree. That is
// the same shape as a conditional branch (`attach_child_at`), which is why the swap reuses the
// scene's `insert_child`/`remove_child` pair instead of a second child-set mechanism.
//
// The model itself is `tessera::ui::pending::SuspenseBoundary` and `TransitionMark`, header-only
// and shared with the IR evaluator, so the differential test compares wiring rather than two copies
// of the rule.
//
// Emission is conditional on the component having a `Suspense` node or a transition-marked action,
// which is what keeps every committed `.g.hpp` golden byte-identical.

#include "emit_events.hpp"
#include "emit_node_groups.hpp"
#include "emit_resources.hpp"
#include "emit_suspense.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::string ordinal_text(unsigned ordinal) {
    return std::to_string(ordinal);
}

// The generated member names of one boundary. `content`/`fallback` are the ownership slots, `live`
// is the attached subtree (null while the other one is attached), and `parent`/`index` are where
// the live subtree belongs.
std::string content_slot(unsigned ordinal) {
    return "suspense_" + ordinal_text(ordinal) + "_content_owned_";
}
std::string fallback_slot(unsigned ordinal) {
    return "suspense_" + ordinal_text(ordinal) + "_fallback_owned_";
}
std::string live_member(unsigned ordinal) {
    return "suspense_" + ordinal_text(ordinal) + "_live_";
}
std::string parent_member(unsigned ordinal) {
    return "suspense_" + ordinal_text(ordinal) + "_parent_";
}
std::string index_member(unsigned ordinal) {
    return "suspense_" + ordinal_text(ordinal) + "_index_";
}
std::string boundary_member(unsigned ordinal) {
    return "suspense_" + ordinal_text(ordinal) + "_";
}
// Which subtree is attached. Tracked as a flag, not inferred from the ownership slots: the live
// subtree's own slot is empty (it was moved into the parent), so a pointer comparison against the
// slots cannot tell "already live" from "not built yet".
std::string live_fallback_member(unsigned ordinal) {
    return "suspense_" + ordinal_text(ordinal) + "_live_fallback_";
}

bool has_transition(const ir::Component& component) {
    for (const ir::Handler& handler : component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (action.transition) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool uses_pending(const ir::Component& component, const DynamicPlan& plan) {
    return !plan.suspenses.empty() || has_transition(component) || uses_resources(component);
}

std::vector<std::string> pending_headers(const ir::Component& component, const DynamicPlan& plan) {
    std::vector<std::string> headers;
    if (!uses_pending(component, plan)) {
        return headers; // no pending surface: no include, so the golden stays byte-identical
    }
    if (!plan.suspenses.empty()) {
        headers.emplace_back("tessera/ui/pending/suspense.hpp");
    }
    if (has_transition(component)) {
        headers.emplace_back("tessera/ui/pending/transition.hpp");
    }
    if (uses_resources(component)) {
        // #656: the boundary reads the resources through the narrow view; no fake resource member.
        headers.emplace_back("tessera/ui/pending/resource_view.hpp");
        return headers;
    }
    // The fake resource backs both halves of the spike: the boundary reads it and a marked setter
    // starts a transition under its epoch.
    headers.emplace_back("tessera/ui/pending/fake_resource.hpp");
    return headers;
}

void emit_pending_fields(CppWriter& writer, const ir::Component& component,
                         const DynamicPlan& plan) {
    if (!uses_pending(component, plan)) {
        return;
    }
    writer.line("// ADR-067 spike #661 (off by default): the pending-UI model. The boundary is a");
    writer.line("// static node, so it owns no scene node: the component owns both subtrees and");
    writer.line("// attaches whichever one the model says is live.");
    if (uses_resources(component)) {
        emit_resource_fields(writer, component);
    } else {
        writer.line("tessera::ui::pending::FakeResource pending_resource_;");
    }
    if (has_transition(component)) {
        writer.line("// The transition mark of a `startTransition` setter (ADR-067 item 4).");
        writer.line("tessera::ui::pending::TransitionMark pending_transition_;");
    }
    for (const SuspenseSite& site : plan.suspenses) {
        writer.line("// `node::" + ordinal_text(site.ordinal) +
                    "` Suspense boundary: the content subtree, the "
                    "fallback subtree,");
        writer.line("// the live one, and where it belongs in the parent.");
        writer.line("std::unique_ptr<tessera::ui::Node> " + content_slot(site.ordinal) + ";");
        writer.line("std::unique_ptr<tessera::ui::Node> " + fallback_slot(site.ordinal) + ";");
        writer.line("tessera::ui::Node* " + live_member(site.ordinal) + " = nullptr;");
        writer.line("bool " + live_fallback_member(site.ordinal) + " = false;");
        writer.line("tessera::ui::Node* " + parent_member(site.ordinal) + " = nullptr;");
        writer.line("std::size_t " + index_member(site.ordinal) + " = 0;");
        writer.line("tessera::ui::pending::SuspenseBoundary " + boundary_member(site.ordinal) +
                    ";");
    }
}

void emit_pending_declarations(CppWriter& writer, const ir::Component& component,
                               const DynamicPlan& plan) {
    if (!uses_pending(component, plan)) {
        return;
    }
    for (const SuspenseSite& site : plan.suspenses) {
        writer.line("// Attaches the subtree the boundary's model says is live, at the boundary's");
        writer.line("// source position. Idempotent: an unchanged phase touches no node.");
        writer.line("void apply_suspense_" + ordinal_text(site.ordinal) + "();");
    }
    if (uses_resources(component)) {
        emit_resource_declarations(writer, component);
        return;
    }
    writer.line("// The fake resource seams (spike #661): the same operations the evaluator's");
    writer.line("// PendingSession exposes, so the differential test plays one script on both.");
    writer.line("std::uint64_t pending_begin_fetch();");
    writer.line("bool pending_resolve(std::uint64_t epoch, int value);");
    writer.line("bool pending_fail(std::uint64_t epoch);");
    writer.line("// Publishes the resource's state to every boundary read and re-applies the");
    writer.line("// boundary: what a `useResource` subscription does at a frame boundary.");
    writer.line("void pending_sync();");
    writer.line("[[nodiscard]] bool pending_is_pending() const;");
    writer.line("[[nodiscard]] bool pending_fallback_live() const;");
    writer.line("[[nodiscard]] std::uint64_t pending_epoch() const;");
    writer.line("[[nodiscard]] int pending_value() const;");
    writer.line("[[nodiscard]] bool pending_loading() const;");
    writer.line("[[nodiscard]] bool pending_has_data() const;");
    writer.line("// The resource's state after a marked change: false when the epoch was stale.");
    writer.line("bool pending_observe_transition();");
    writer.line("// The marked setter, as the Button's click binding runs it (the differential");
    writer.line("// test drives the same path the scene does).");
    writer.line("void run_marked_set();");
}

bool emit_pending_sources(CppWriter& writer, const ir::Component& component,
                          const DynamicPlan& plan, std::string& error) {
    if (!uses_pending(component, plan)) {
        return true;
    }
    for (const SuspenseSite& site : plan.suspenses) {
        writer.open("void " + component.name + "::apply_suspense_" + ordinal_text(site.ordinal) +
                    "()");
        writer.line("tessera::ui::Node* parent = " + parent_member(site.ordinal) + ";");
        writer.line("if (parent == nullptr) {");
        ++writer.indent;
        writer.line("return;");
        --writer.indent;
        writer.line("}");
        writer.line("const bool fallback = " + boundary_member(site.ordinal) + ".fallback_live();");
        writer.line("tessera::ui::Node*& live = " + live_member(site.ordinal) + ";");
        writer.line("bool& live_fallback = " + live_fallback_member(site.ordinal) + ";");
        writer.line("if (live != nullptr && live_fallback == fallback) {");
        ++writer.indent;
        writer.line("return; // the phase did not change: no node API is touched");
        --writer.indent;
        writer.line("}");
        writer.line("if (live != nullptr) {");
        ++writer.indent;
        writer.line("// Detach the subtree that is going away, keeping it alive for a later swap.");
        writer.line("std::unique_ptr<tessera::ui::Node> previous = parent->remove_child(*live);");
        writer.line("if (live_fallback) {");
        ++writer.indent;
        writer.line(fallback_slot(site.ordinal) + " = std::move(previous);");
        --writer.indent;
        writer.line("} else {");
        ++writer.indent;
        writer.line(content_slot(site.ordinal) + " = std::move(previous);");
        --writer.indent;
        writer.line("}");
        writer.line("live = nullptr;");
        --writer.indent;
        writer.line("}");
        writer.line("std::unique_ptr<tessera::ui::Node>& wanted =");
        writer.line("    fallback ? " + fallback_slot(site.ordinal) + " : " +
                    content_slot(site.ordinal) + ";");
        writer.line("if (!wanted) {");
        ++writer.indent;
        writer.line("return;");
        --writer.indent;
        writer.line("}");
        writer.line("const std::size_t at = " + index_member(site.ordinal) +
                    " < "
                    "parent->children().size()");
        writer.line("                         ? " + index_member(site.ordinal) +
                    " : parent->children().size();");
        writer.line("parent->insert_child(at, std::move(wanted));");
        writer.line("live = parent->children()[at].get();");
        writer.line("live_fallback = fallback;");
        writer.close("");
        writer.line("");
    }
    if (uses_resources(component)) {
        return emit_resource_sources(writer, component, plan, error);
    }
    writer.open("std::uint64_t " + component.name + "::pending_begin_fetch()");
    writer.line("return pending_resource_.begin_fetch();");
    writer.close("");
    writer.line("");
    writer.open("bool " + component.name + "::pending_resolve(std::uint64_t epoch, int value)");
    writer.line("return pending_resource_.resolve(epoch, value);");
    writer.close("");
    writer.line("");
    writer.open("bool " + component.name + "::pending_fail(std::uint64_t epoch)");
    writer.line("return pending_resource_.fail(epoch);");
    writer.close("");
    writer.line("");
    writer.open("void " + component.name + "::pending_sync()");
    for (const SuspenseSite& site : plan.suspenses) {
        writer.line("// `node::" + ordinal_text(site.ordinal) + "`: one suspending read.");
        writer.line("(void)" + boundary_member(site.ordinal) +
                    ".set_read(0, pending_resource_.loading(),");
        writer.line("                              pending_resource_.has_data(),");
        writer.line("                              pending_resource_.epoch());");
        writer.line("apply_suspense_" + ordinal_text(site.ordinal) + "();");
    }
    if (plan.suspenses.empty()) {
        writer.line("(void)pending_resource_;");
    }
    writer.close("");
    writer.line("");
    writer.open("bool " + component.name + "::pending_is_pending() const");
    if (has_transition(component)) {
        writer.line("return pending_transition_.pending();");
    } else {
        writer.line("return false;");
    }
    writer.close("");
    writer.line("");
    writer.open("bool " + component.name + "::pending_fallback_live() const");
    if (plan.suspenses.empty()) {
        writer.line("return false;");
    } else {
        writer.line("return " + boundary_member(plan.suspenses.front().ordinal) +
                    ".fallback_live();");
    }
    writer.close("");
    writer.line("");
    writer.open("std::uint64_t " + component.name + "::pending_epoch() const");
    writer.line("return pending_resource_.epoch();");
    writer.close("");
    writer.line("");
    writer.open("int " + component.name + "::pending_value() const");
    writer.line("return pending_resource_.value();");
    writer.close("");
    writer.line("");
    writer.open("bool " + component.name + "::pending_loading() const");
    writer.line("return pending_resource_.loading();");
    writer.close("");
    writer.line("");
    writer.open("bool " + component.name + "::pending_has_data() const");
    writer.line("return pending_resource_.has_data();");
    writer.close("");
    writer.line("");
    writer.open("bool " + component.name + "::pending_observe_transition()");
    if (has_transition(component)) {
        writer.line("return pending_transition_.observe(pending_resource_.loading(),");
        writer.line("                                pending_resource_.epoch());");
    } else {
        writer.line("return false;");
    }
    writer.close("");
    writer.line("");
    return emit_run_marked_set(writer, component, true, error);
}

bool emit_run_marked_set(CppWriter& writer, const ir::Component& component, bool fake_resource,
                         std::string& error) {
    // The marked setter, callable without a scene event: the differential test drives the same
    // generated handler body the Button's click binding runs. The payload is the handler's own
    // typed record, converted from a bare event exactly as the scene adapter does it.
    writer.open("void " + component.name + "::run_marked_set()");
    bool emitted = false;
    for (const ir::Handler& handler : component.handlers) {
        bool marked = false;
        for (const ir::Action& action : handler.actions) {
            marked = marked || action.transition;
        }
        if (!marked) {
            continue;
        }
        bool typed = false;
        std::string param_name;
        if (!payload_signature(handler.params, typed, param_name)) {
            return fail(error, "handler `" + handler.id + "` has an unsupported payload record");
        }
        std::string argument = "tessera::ui::Event{}";
        if (typed) {
            const PayloadRecord* payload = find_payload(handler.params.front().type);
            if (payload == nullptr) {
                return fail(error, "handler `" + handler.id + "` payload record `" +
                                       handler.params.front().type + "` has no C++ record");
            }
            argument = "tessera::ui::" + std::string(payload->converter) + "(tessera::ui::Event{})";
        }
        writer.line(method_name(handler.id) + "(" + argument + ");");
        emitted = true;
    }
    if (!emitted) {
        if (fake_resource) {
            writer.line("(void)pending_resource_;");
        }
    }
    writer.close("");
    writer.line("");
    return true;
}

bool emit_suspense_children(NodeEmitContext& context) {
    const ir::Node& node = context.node;
    if (node.children.size() != 1 || !node.fallback) {
        return fail(context.error, "a Suspense boundary needs one child subtree and a fallback");
    }
    const SuspenseSite* site = nullptr;
    for (const SuspenseSite& candidate : context.state.plan->suspenses) {
        if (candidate.ordinal == context.ordinal) {
            site = &candidate;
            break;
        }
    }
    if (site == nullptr) {
        return fail(context.error,
                    "the Suspense boundary `" + node.id + "` is missing from the dynamic plan");
    }
    // The content subtree, then the fallback subtree, in the order the plan walk numbered them.
    std::string content_variable;
    if (!emit_node(context.writer, node.children[0], context.state, content_variable,
                   context.error)) {
        return false;
    }
    context.writer.line("this->" + content_slot(site->ordinal) + " = std::move(" +
                        content_variable + ");");
    std::string fallback_variable;
    if (!emit_node(context.writer, *node.fallback, context.state, fallback_variable,
                   context.error)) {
        return false;
    }
    context.writer.line("this->" + fallback_slot(site->ordinal) + " = std::move(" +
                        fallback_variable + ");");
    // The boundary is static: it records where its live subtree belongs and attaches it now. The
    // parent variable is a `unique_ptr` in build() and a `Node*` inside an item builder, so
    // `.get()` is what makes both spellings work.
    context.writer.line("this->" + parent_member(site->ordinal) + " = " +
                        context.state.parent_variable + ".get();");
    context.writer.line("this->" + index_member(site->ordinal) + " = " +
                        context.state.parent_variable + "->children().size();");
    const std::size_t read_count = uses_resources(context.state.component)
                                       ? boundary_reads(context.state.component, node).size()
                                       : site->read_count;
    context.writer.line("this->" + boundary_member(site->ordinal) + ".reset_reads(" +
                        std::to_string(read_count) + ");");
    // A resource-driven component publishes the bound views first (a host may bind before build()),
    // which also applies this boundary; the others have nothing to read yet.
    context.writer.line(uses_resources(context.state.component)
                            ? "this->pending_sync();"
                            : "this->apply_suspense_" + ordinal_text(site->ordinal) + "();");
    context.variable.clear(); // the caller attaches nothing: the boundary attached its subtree
    return true;
}

} // namespace tessera::compiler::codegen::internal
