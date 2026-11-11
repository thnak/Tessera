// SPDX-License-Identifier: MIT

#include "emit_layout_observe.hpp"

#include <algorithm>
#include <cstddef>

namespace tessera::compiler::codegen::internal {
namespace {

constexpr int kMaxCompositionDepth = 32;

const ir::Component* find_component_by_id(const ir::IrModule& module, std::string_view id) {
    for (const ir::Component& candidate : module.components) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

bool node_tree_observes(const ir::Node& node, const ir::IrModule& module, int depth);

bool component_observes(const ir::Component& component, const ir::IrModule& module, int depth) {
    return depth <= kMaxCompositionDepth && node_tree_observes(component.root, module, depth);
}

bool node_tree_observes(const ir::Node& node, const ir::IrModule& module, int depth) {
    if (node.kind == ir::NodeKind::Component) {
        // The host node of an owned instance: its own events are callback props, never scene
        // events, so only the instantiated component can observe.
        const ir::Component* target = find_component_by_id(module, node.component);
        if (target != nullptr && component_observes(*target, module, depth + 1)) {
            return true;
        }
    } else if (node.events.count("onSizeChange") != 0) {
        return true;
    }
    for (const ir::Node& child : node.children) {
        if (node_tree_observes(child, module, depth)) {
            return true;
        }
    }
    return node.item_template && node_tree_observes(*node.item_template, module, depth);
}

bool instance_observes(const ComponentInstance& instance, const ir::IrModule& module) {
    const ir::Component* target = find_component_by_id(module, instance.component_id);
    return target != nullptr && component_observes(*target, module, 1);
}

// One attach/detach step in mount order: an owned observation or a forwarding instance.
struct Step {
    unsigned ordinal = 0;
    const ComponentInstance* instance = nullptr; // null for an own observation
};

std::vector<Step> mount_steps(const ir::IrModule& module, const std::vector<EventAdapter>& adapters,
                              const std::vector<ComponentInstance>& instances) {
    std::vector<Step> steps;
    for (const EventAdapter& adapter : adapters) {
        if (adapter.size_change) {
            steps.push_back(Step{adapter.ordinal, nullptr});
        }
    }
    for (const ComponentInstance& instance : instances) {
        if (instance_observes(instance, module)) {
            steps.push_back(Step{instance.ordinal, &instance});
        }
    }
    std::stable_sort(steps.begin(), steps.end(),
                     [](const Step& a, const Step& b) { return a.ordinal < b.ordinal; });
    return steps;
}

} // namespace

bool observes_size(const ir::Component& component, const ir::IrModule& module) {
    return component_observes(component, module, 0);
}

std::string size_observation_member(unsigned ordinal) {
    return "size_observation_" + std::to_string(ordinal) + "_";
}

void emit_size_observation_declarations(CppWriter& writer, const ir::Component& component,
                                        const ir::IrModule& module,
                                        const std::vector<EventAdapter>& adapters) {
    if (!observes_size(component, module)) {
        return;
    }
    writer.line(
        "// Layout observation (onSizeChange): one observation per observed node. The host's");
    writer.line("// LayoutObserverSet delivers it after layout; the component must not move while");
    writer.line("// attached.");
    writer.line("tessera::ui::layout::LayoutObserverSet* layout_observers_ = nullptr;");
    for (const EventAdapter& adapter : adapters) {
        if (adapter.size_change) {
            writer.line("std::optional<tessera::ui::layout::LayoutObservation> " +
                        size_observation_member(adapter.ordinal) + ";");
        }
    }
    writer.line("void attach_layout_observers(tessera::ui::layout::LayoutObserverSet& observers);");
    writer.line("void detach_layout_observers(tessera::ui::layout::LayoutObserverSet& observers);");
}

void emit_size_observation_definitions(CppWriter& writer, const ir::Component& component,
                                       const ir::IrModule& module,
                                       const std::vector<EventAdapter>& adapters,
                                       const std::vector<ComponentInstance>& instances) {
    if (!observes_size(component, module)) {
        return;
    }
    const std::vector<Step> steps = mount_steps(module, adapters, instances);
    writer.open("void " + component.name +
                "::attach_layout_observers(tessera::ui::layout::LayoutObserverSet& observers)");
    writer.line("layout_observers_ = &observers;");
    for (const Step& step : steps) {
        if (step.instance == nullptr) {
            writer.open("if (" + size_observation_member(step.ordinal) + ")");
            writer.line("observers.attach(*" + size_observation_member(step.ordinal) + ");");
            writer.close("");
        } else {
            writer.line(instance_member(step.ordinal) + ".attach_layout_observers(observers);");
        }
    }
    writer.close("");
    writer.line("");

    writer.open("void " + component.name +
                "::detach_layout_observers(tessera::ui::layout::LayoutObserverSet& observers)");
    for (std::size_t i = steps.size(); i-- > 0;) {
        const Step& step = steps[i];
        if (step.instance == nullptr) {
            writer.open("if (" + size_observation_member(step.ordinal) + ")");
            writer.line("observers.detach(*" + size_observation_member(step.ordinal) + ");");
            writer.close("");
        } else {
            writer.line(instance_member(step.ordinal) + ".detach_layout_observers(observers);");
        }
    }
    writer.line("layout_observers_ = nullptr;");
    writer.close("");
    writer.line("");
}

void emit_size_observation_binding(CppWriter& writer, std::string_view variable,
                                   const EventAdapter& adapter) {
    const std::string member = size_observation_member(adapter.ordinal);
    writer.line("this->" + member + ".emplace(*" + std::string(variable) + ", &" +
                adapter.function + ", this);");
    writer.open("if (this->layout_observers_ != nullptr)");
    writer.line("this->layout_observers_->attach(*this->" + member + ");");
    writer.close("");
}

} // namespace tessera::compiler::codegen::internal
