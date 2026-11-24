// SPDX-License-Identifier: MIT

// The provider push of the static context form (ADR-066 item 5, issue #802): the provider half of
// `apply_props()`. A component that renders `<Ctx.Provider value={e}>` evaluates `e` — a pure
// expression of its props and state — and pushes it into every instance below it whose component
// consumes `Ctx`, so the nearest provider wins without a runtime walk: the parent knows its
// children. A consumer under no provider keeps the `createContext` default its IR environment
// entry carries, which is what the field is initialized with.

#include "emit_props.hpp"
#include "value_ops.hpp"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

// The provider nodes of one component's tree, by context name. The nearest provider wins without a
// runtime walk: this map is what `apply_props()` pushes from, and a nested provider of the same
// context overwrites the outer one during the pre-order walk, which is the shadowing rule React's
// tree gives.
void collect_providers(const ir::Node& node, std::map<std::string, const ir::Node*>& out) {
    if (!node.provider.empty()) {
        out[node.provider] = &node;
    }
    for (const ir::Node& child : node.children) {
        collect_providers(child, out);
    }
    if (node.fallback) {
        collect_providers(*node.fallback, out);
    }
    if (node.item_template) {
        collect_providers(*node.item_template, out);
    }
}

} // namespace

bool collect_providers(const ir::Component& owner, std::map<std::string, const ir::Node*>& out) {
    collect_providers(owner.root, out);
    return !out.empty();
}

bool emit_provider_pushes(CppWriter& writer, const ir::Component& owner, const ir::IrModule& module,
                          const std::map<std::string, const ir::Node*>& provided,
                          const std::vector<ComponentInstance>& instances, std::string& error) {
    if (provided.empty()) {
        return true;
    }
    // The provider pushes first: a consumer's first frame reads the provided value, not the
    // default, before its own props move it.
    for (const ComponentInstance& instance : instances) {
        if (instance.in_template()) {
            // A per-item instance's context values follow its item's lifecycle; the item builder
            // would need the provider's locals, which this slice defers.
            continue;
        }
        const ir::Component* target = find_module_component(module, instance.component_id);
        if (target == nullptr) {
            continue;
        }
        for (const ir::Environment& env : target->environment) {
            if (env.provider.rfind("context:", 0) != 0) {
                continue;
            }
            const std::string context = env.provider.substr(8);
            const auto provided_node = provided.find(context);
            if (provided_node == provided.end() ||
                provided_node->second->provider_value == nullptr) {
                // No provider here: the consumer keeps the createContext default. A provider in a
                // *different* module is the walk's TESSERA_REACT_CONTEXT_DYNAMIC case.
                continue;
            }
            const ExprEmission value =
                emit_initializer_expr(*provided_node->second->provider_value, owner);
            if (!value.error.empty()) {
                error = "the value of provider `" + context + "`: " + value.error;
                return false;
            }
            writer.line("// context `" + context + "` -> " + instance.component_id +
                        " instance (pre-order index " + std::to_string(instance.ordinal) + ").");
            writer.line("this->" + instance_member(instance.ordinal) + ".set_environment_" +
                        env.name + "(" + value.text + ");");
        }
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
