// SPDX-License-Identifier: MIT

// Component composition and the static `Children` slot (docs/specs/ir-v1.md section 3.4,
// docs/design/compiler.md section 11). A `Component` node becomes a generated host node plus one
// member of the referenced component's generated struct; the caller's slot content is built by the
// caller's code and handed to the callee's `build()`, which attaches it at its `Slot` node. Each
// instance keeps its own state, so a referenced component is never inlined.

#include "codegen_internal.hpp"
#include "emit_events.hpp"
#include "emit_layout_observe.hpp"
#include "emit_node.hpp"
#include "emit_props.hpp"
#include "form_binding.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

const ir::Component* find_component(const ir::IrModule& module, std::string_view id) {
    for (const ir::Component& component : module.components) {
        if (component.id == id) {
            return &component;
        }
    }
    return nullptr;
}

bool declares_children(const ir::Component& component) {
    for (const ir::Prop& prop : component.props) {
        if (prop.name == "children" && prop.type == "children") {
            return true;
        }
    }
    return false;
}

// The first-party form components own caller-side controller wiring (form_binding.hpp) that is
// planned per owning component, so a per-item instance of one is deferred (loud diagnostic).
bool form_free(const ir::Component& target) {
    return target.name != "Form" && target.name != "Field";
}

// Walks a node tree and appends every `Component` node to `instances`; returns false after the
// first deferred form. `template_ordinal` is the enclosing list template's loop-node ordinal (0
// outside a template): such an instance is owned by the keyed item record, one per item.
bool collect_instances(const ir::IrModule& module, const ir::Component& owner, const ir::Node& node,
                       unsigned template_ordinal, unsigned& ordinal,
                       std::vector<ComponentInstance>& instances, std::string& error) {
    const unsigned self = ++ordinal;
    if (node.loop) {
        template_ordinal = self;
    }
    const bool instance = node.kind == ir::NodeKind::Component;
    if (instance) {
        const ir::Component* target = find_component(module, node.component);
        if (target == nullptr) {
            return fail(error, "the component instance `" + node.id +
                                   "` names unknown component `" + node.component + "`");
        }
        const bool in_template = template_ordinal != 0;
        if (in_template && !node.children.empty()) {
            return fail(error, "slot children of a component instance (`" + node.id +
                                   "`) inside a list template are deferred; wrap the content in an "
                                   "element");
        }
        if (in_template && observes_size(*target, module)) {
            return fail(error, "the component instance `" + node.id +
                                   "` observes its size (`onSizeChange`) and is inside a list "
                                   "template; per-item layout observers are deferred");
        }
        if (in_template && !form_free(*target)) {
            return fail(error, "the form component instance `" + node.id +
                                   "` inside a list template is deferred; a per-item form binding "
                                   "needs per-item controller storage");
        }
        const bool takes = declares_children(*target);
        // Props are validated against the target's declared prop records; the errors carry the
        // prop name (docs/specs/component-model-v1.md section 3).
        ComponentInstance entry{self, node.component, target->name, takes, {}};
        entry.template_ordinal = template_ordinal;
        if (!collect_instance_props(module, owner, node, *target, entry, error)) {
            return false;
        }
        instances.push_back(std::move(entry));
        if (!node.children.empty() && !takes) {
            return fail(error, "the component instance `" + node.id + "` has slot children, but `" +
                                   node.component + "` does not declare a `children` prop");
        }
        for (const ir::Node& child : node.children) {
            if (child.condition || child.loop) {
                return fail(error, "a conditional or list node directly in slot content (`" +
                                       child.id + "`) is deferred; wrap the content in an element");
            }
        }
    }
    for (const ir::Node& child : node.children) {
        if (!collect_instances(module, owner, child, template_ordinal, ordinal, instances, error)) {
            return false;
        }
    }
    if (node.fallback && !collect_instances(module, owner, *node.fallback, template_ordinal,
                                            ordinal, instances, error)) {
        return false;
    }
    if (node.item_template && !collect_instances(module, owner, *node.item_template,
                                                 template_ordinal, ordinal, instances, error)) {
        return false;
    }
    return true;
}

void collect_include_names(const ir::IrModule& module, const ir::Node& node,
                           std::vector<std::string>& names) {
    if (node.kind == ir::NodeKind::Component) {
        const ir::Component* target = find_component(module, node.component);
        if (target != nullptr && target->name != "") {
            names.push_back(target->name);
        }
    }
    for (const ir::Node& child : node.children) {
        collect_include_names(module, child, names);
    }
    if (node.fallback) {
        collect_include_names(module, *node.fallback, names);
    }
    if (node.item_template) {
        collect_include_names(module, *node.item_template, names);
    }
}

void collect_edge_targets(const ir::Node& node, std::vector<std::string>& edges) {
    if (node.kind == ir::NodeKind::Component) {
        edges.push_back(node.component);
    }
    for (const ir::Node& child : node.children) {
        collect_edge_targets(child, edges);
    }
    if (node.fallback) {
        collect_edge_targets(*node.fallback, edges);
    }
    if (node.item_template) {
        collect_edge_targets(*node.item_template, edges);
    }
}

bool visit_reference(const ir::IrModule& module, std::size_t index, std::vector<int>& color,
                     std::string& error) {
    constexpr int kOnStack = 1;
    constexpr int kDone = 2;
    int& state = color[index];
    if (state == kOnStack) {
        return fail(error, "component reference cycle: `" + module.components[index].id +
                               "` reaches itself; the IR validator rejects it (TESSERA_IR_CYCLE)");
    }
    if (state == kDone) {
        return true;
    }
    state = kOnStack;
    std::vector<std::string> edges;
    collect_edge_targets(module.components[index].root, edges);
    for (const std::string& edge : edges) {
        const auto found =
            std::find_if(module.components.begin(), module.components.end(),
                         [&](const ir::Component& component) { return component.id == edge; });
        if (found == module.components.end()) {
            return fail(error, "a component instance names unknown component `" + edge + "`");
        }
        const std::size_t next =
            static_cast<std::size_t>(std::distance(module.components.begin(), found));
        if (!visit_reference(module, next, color, error)) {
            return false;
        }
    }
    state = kDone;
    return true;
}

} // namespace

const ir::Component* find_module_component(const ir::IrModule& module, std::string_view id) {
    return find_component(module, id);
}

bool component_takes_children(const ir::Component& component) {
    return declares_children(component);
}

std::string instance_member(unsigned ordinal) {
    return "child_" + std::to_string(ordinal) + "_";
}

bool collect_component_instances(const ir::IrModule& module, const ir::Component& component,
                                 std::vector<ComponentInstance>& instances, std::string& error) {
    unsigned ordinal = 0;
    return collect_instances(module, component, component.root, 0, ordinal, instances, error);
}

std::vector<std::string> component_includes(const ir::IrModule& module,
                                            const ir::Component& component) {
    std::vector<std::string> names;
    collect_include_names(module, component.root, names);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool reject_component_cycle(const ir::IrModule& module, std::string& error) {
    std::vector<int> color(module.components.size(), 0);
    for (std::size_t i = 0; i < module.components.size(); ++i) {
        if (!visit_reference(module, i, color, error)) {
            return true;
        }
    }
    return false;
}

bool emit_component_node(CppWriter& writer, const ir::Node& node, NodeEmitState& state,
                         std::string& variable, std::string& error) {
    if (state.module == nullptr) {
        return fail(error, "component composition needs the module; this is an internal error");
    }
    const ir::Component* target = find_component(*state.module, node.component);
    if (target == nullptr) {
        return fail(error, "the component instance (`" + node.id + "`) names unknown component `" +
                               node.component + "`");
    }
    // A conditional instance keeps its instance in a branch slot: the host node this function
    // returns is stored by the owner's child walk instead of attached
    // (docs/design/compiler-node-state.md section 2). The instance itself is built, mounted and
    // updated exactly once. Inside an item builder, a `loop` on this node is the template marker
    // and belongs to `build_item_<n>`, not to the instance.
    if (node.loop && state.item_out.empty()) {
        return fail(error, "a looped component instance (`" + node.id +
                               "`) is deferred; wrap the element in a container");
    }
    if (!node.style.empty()) {
        return fail(error, "a Component node carries props, not style (`" + node.id + "`)");
    }
    const bool takes = component_takes_children(*target);
    if (!node.children.empty() && !takes) {
        return fail(error, "the component instance `" + node.id + "` has slot children, but `" +
                               node.component + "` does not declare a `children` prop");
    }
    std::string id = node_id_text(node, state, error);
    if (!error.empty()) {
        return false;
    }
    const unsigned ordinal = state.next_var;
    variable = "node_" + std::to_string(state.next_var++);
    writer.line("// " + node.id + " (\"" + node.component +
                "\" instance, slot children: " + std::to_string(node.children.size()) + ")");
    writer.line("auto " + variable + " = std::make_unique<tessera::ui::Node>(" + id + ");");
    writer.line(variable + "->set_element_kind(\"Component\");");
    // The component name types the host for selector matching: the host side of a `::part()`
    // selector names the component (`Button::part(label)`, stylesheet-v1 section 6.3).
    writer.line(variable + "->set_component_name(\"" + target->name + "\");");
    if (!node.automation_id.empty()) {
        unsigned long long automation = 0;
        const char* begin = node.automation_id.data();
        const auto parsed = std::from_chars(begin, begin + node.automation_id.size(), automation);
        if (parsed.ec != std::errc{} || parsed.ptr != begin + node.automation_id.size()) {
            return fail(error, "node `" + node.id + "` has a non-numeric automationId `" +
                                   node.automation_id + "`");
        }
        writer.line(variable + "->set_automation_id(" + std::to_string(automation) + "ull);");
    }
    for (const ir::Ref& ref : state.component.refs) {
        if (ref.target == node.id) {
            if (!state.item_out.empty()) {
                return fail(error, "a `useRef` target (`" + ref.id +
                                       "`) on a component instance inside a list template is "
                                       "deferred; one ref cannot name every item's instance");
            }
            writer.line("this->" + member_name(ref.id) + " = " + variable + ".get();");
        }
    }
    if (!state.item_out.empty()) {
        // Per-item instance (docs/design/compiler-node-state.md section 3): the item record owns
        // it, so its state lives as long as the item key does. Props are the callee's build inputs,
        // so they are assigned before build(); a callback lambda captures `this`, the item's owner.
        const std::string field = state.item_out + ".child_" + std::to_string(ordinal);
        const ComponentInstance* collected = nullptr;
        if (state.instances != nullptr) {
            for (const ComponentInstance& candidate : *state.instances) {
                if (candidate.ordinal == ordinal) {
                    collected = &candidate;
                    break;
                }
            }
        }
        if (collected == nullptr) {
            return fail(error, "the component instance `" + node.id +
                                   "` is missing from the collected instances; this is an internal "
                                   "codegen error");
        }
        writer.line(field + " = std::make_unique<" + target->name + ">();");
        if (state.locals == nullptr) {
            return fail(error, "a per-item instance needs the item builder locals");
        }
        if (!emit_item_instance_props(writer, state.component, *collected, state.item_out, field,
                                      *state.locals, true, error)) {
            return false;
        }
        // Distinct ids per item and per instance, as for a static instance (issue #295); the host
        // id includes the item index.
        writer.line(field + "->node_id_base_ = " + id + " * tessera::ui::kNodeIdBaseStride;");
        writer.line(field + "->build(*" + variable + ");");
        // Lifecycle and the update scheduler follow the owner: an item created after the owner was
        // mounted starts mounted, and its ticket joins the scheduler the owner was given.
        writer.line("if (this->mounted_) {");
        writer.indent += 1;
        writer.line(field + "->mount();");
        writer.indent -= 1;
        writer.line("}");
        writer.line("if (this->scheduler_ != nullptr) {");
        writer.indent += 1;
        writer.line(field + "->attach_scheduler(*this->scheduler_);");
        writer.indent -= 1;
        writer.line("}");
        return true;
    }
    // The caller's slot content: nodes owned by the caller, attached by the callee at its Slot.
    const std::string slot = "slot_" + std::to_string(ordinal);
    writer.line("std::vector<std::unique_ptr<tessera::ui::Node>> " + slot + ";");
    // A Field bound to a Form controller records its first slot node for focus-first-invalid and,
    // for a text widget, binds the generated edit adapter that routes the committed text change
    // into the controller (form_binding.hpp).
    const FormFieldBinding* form_field =
        state.forms == nullptr ? nullptr : state.forms->field(ordinal);
    bool first_child = true;
    for (const ir::Node& child : node.children) {
        std::string child_variable;
        if (!emit_node(writer, child, state, child_variable, error)) {
            return false;
        }
        if (form_field != nullptr && first_child) {
            first_child = false;
            writer.line("this->" +
                        form_field_member(form_field->form_ordinal, form_field->symbol, "widget") +
                        " = " + child_variable + ".get();");
            if (form_field->text_widget) {
                writer.line(child_variable + "->bind_event(tessera::ui::EventType::TextChange, &" +
                            form_field_adapter(form_field->form_ordinal, form_field->symbol) +
                            ", this);");
            }
        }
        writer.line(slot + ".push_back(std::move(" + child_variable + "));");
    }
    const std::string member = instance_member(ordinal);
    // Every instance of one component would build the same baked NodeIds, and focus, capture and
    // click synthesis resolve nodes by id. The host's id (which already includes this component's
    // own base) is unique here, so it seeds the instance's base before any node is created
    // (issue #295).
    writer.line("this->" + member + ".node_id_base_ = " + id +
                " * tessera::ui::kNodeIdBaseStride;");
    if (takes) {
        writer.line("this->" + member + ".build(*" + variable + ", std::move(" + slot + "));");
    } else {
        writer.line("this->" + member + ".build(*" + variable + ");");
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
