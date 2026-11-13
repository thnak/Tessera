// SPDX-License-Identifier: MIT

// Component composition and the static `Children` slot (docs/specs/ir-v1.md section 3.4,
// docs/design/compiler.md section 11). A `Component` node becomes a generated host node plus one
// member of the referenced component's generated struct; the caller's slot content is built by the
// caller's code and handed to the callee's `build()`, which attaches it at its `Slot` node. Each
// instance keeps its own state, so a referenced component is never inlined.

#include "codegen_internal.hpp"
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

// Walks a node tree and appends every `Component` node to `instances`; returns false after the
// first deferred form.
bool collect_instances(const ir::IrModule& module, const ir::Component& owner, const ir::Node& node,
                       bool in_template, unsigned& ordinal,
                       std::vector<ComponentInstance>& instances, std::string& error) {
    const unsigned self = ++ordinal;
    if (node.loop) {
        in_template = true;
    }
    const bool instance = node.kind == ir::NodeKind::Component;
    if (instance) {
        if (in_template) {
            return fail(error, "a component instance (`" + node.id +
                                   "`) inside a list template is deferred; wrap the list in an "
                                   "element");
        }
        if (node.condition || node.loop) {
            return fail(error, "a conditional or looped component instance (`" + node.id +
                                   "`) is deferred; wrap the element in a container");
        }
        const ir::Component* target = find_component(module, node.component);
        if (target == nullptr) {
            return fail(error, "the component instance `" + node.id +
                                   "` names unknown component `" + node.component + "`");
        }
        const bool takes = declares_children(*target);
        // Props are validated against the target's declared prop records; the errors carry the
        // prop name (docs/specs/component-model-v1.md section 3).
        ComponentInstance entry{self, node.component, target->name, takes, {}};
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
        if (!collect_instances(module, owner, child, in_template, ordinal, instances, error)) {
            return false;
        }
    }
    if (node.fallback &&
        !collect_instances(module, owner, *node.fallback, in_template, ordinal, instances, error)) {
        return false;
    }
    if (node.item_template && !collect_instances(module, owner, *node.item_template, in_template,
                                                 ordinal, instances, error)) {
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
    return collect_instances(module, component, component.root, false, ordinal, instances, error);
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
    if (node.condition || node.loop) {
        return fail(error, "a conditional or looped component instance (`" + node.id +
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
            writer.line("this->" + member_name(ref.id) + " = " + variable + ".get();");
        }
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
    if (takes) {
        writer.line("this->" + member + ".build(*" + variable + ", std::move(" + slot + "));");
    } else {
        writer.line("this->" + member + ".build(*" + variable + ");");
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
