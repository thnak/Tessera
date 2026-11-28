// SPDX-License-Identifier: MIT

// Component-level checks (docs/specs/ir-v1.md sections 2, 2.1 and 3.1).

#include "validate_internal.hpp"

#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace tessera::compiler::ir {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

struct ComponentChecker {
    ValidationContext& context;
    const Component& component;
    ComponentScope scope;

    // Declares an id that `ref` expressions can name, reporting a duplicate.
    void declare(const std::string& id, std::string_view prefix, const SourceSpan& where) {
        if (!starts_with(id, prefix) || id.size() == prefix.size()) {
            context.report("TESSERA_IR_MALFORMED",
                           "id `" + id + "` must look like `" + std::string(prefix) + "name`",
                           where);
        }
        if (!scope.ids.insert(id).second) {
            context.report("TESSERA_IR_DUPLICATE_ID", "duplicate id `" + id + "`", where);
        }
    }

    void check_declarations();
    void check_handlers();
    void check_effects_and_refs();
    bool resolve_binding(const std::string& id, const std::string& what);
    bool resolve_effect_binding(const std::string& id, const std::string& what);
};

void ComponentChecker::check_declarations() {
    const SourceSpan& where = component.source;
    std::unordered_set<std::string> prop_names;
    for (const auto& prop : component.props) {
        if (!prop_names.insert(prop.name).second) {
            context.report("TESSERA_IR_DUPLICATE_ID", "duplicate prop `" + prop.name + "`", where);
        }
        // Section 3.4: `children` is the one prop with the `children` slot type, and the type is
        // valid only for that prop. A default value has no meaning for a slot.
        const bool children_prop = prop.name == "children";
        if (children_prop && prop.type != "children") {
            context.report("TESSERA_IR_SLOT",
                           "the `children` prop declares the slot, so its type is `children`",
                           where);
        } else if (!children_prop && prop.type == "children") {
            context.report("TESSERA_IR_SLOT", "only the `children` prop has type `children`",
                           where);
        }
        if (children_prop && prop.default_value) {
            context.report("TESSERA_IR_SLOT", "the `children` prop has no default value", where);
        }
        if (children_prop && prop.optional) {
            context.report("TESSERA_IR_SLOT", "the `children` prop has no optional form", where);
        }
        validate_type(context, prop.type, where);
        // Section 4.3: a prop default is one of the three positions a record value is allowed in.
        validate_record_position(context, scope, prop.default_value.get(), prop.type, where);
    }
    for (const auto& state : component.state) {
        declare(state.id, "state::", where);
        validate_type(context, state.type, where);
        scope.declared_types.emplace(state.id, state.type);
        // Section 4.3: a `useState` initial is one of the three positions a record value may take.
        if (state.initial == nullptr) {
            context.report("TESSERA_IR_MALFORMED", "initial value of " + state.id + " is missing",
                           where);
        } else {
            validate_record_position(context, scope, state.initial.get(), state.type, where);
        }
        // ADR-066 item 4: a reducer state's dispatch table. Each case's handler must be a declared
        // handler of this component, and a `default` case is at most one.
        bool seen_default = false;
        for (const DispatchCase& entry : state.dispatch) {
            if (entry.action.empty()) {
                if (seen_default) {
                    context.report("TESSERA_IR_MALFORMED",
                                   "`" + state.id + "` has more than one `default` dispatch case",
                                   where);
                }
                seen_default = true;
            } else {
                scope.dispatch[state.id].insert(entry.action);
            }
            if (!starts_with(entry.handler, "handler::")) {
                context.report("TESSERA_IR_MALFORMED",
                               "dispatch case handler `" + entry.handler + "` is not a handler id",
                               where);
            }
        }
    }
    for (const auto& value : component.animated_values) {
        declare(value.id, "anim::", where);
        validate_type(context, value.type, where);
        scope.declared_types.emplace(value.id, value.type);
        require_expr(context, scope, {}, value.initial.get(), where,
                     "initial value of " + value.id);
    }
    for (const auto& env : component.environment) {
        declare(env.id, "env::", env.source);
        validate_type(context, env.type, env.source);
        scope.declared_types.emplace(env.id, env.type);
        validate_span(context, env.source, "environment `" + env.id + "`");
        if (env.provider.empty()) {
            context.report("TESSERA_IR_MALFORMED", "environment `" + env.id + "` has no provider",
                           env.source);
        }
    }
    // ADR-064 item 7: a behavior is a `beh::<name>` id, a registry name, zero or more target refs
    // (`useHotkeys` and `useTransition` take none) and one arg per manifest field. The registry
    // name itself is checked at generation time
    // (`TESSERA_BEHAVIOR_UNKNOWN`), because the IR validator has no registry.
    for (const auto& behavior : component.behaviors) {
        declare(behavior.id, "beh::", behavior.source);
        validate_span(context, behavior.source, "behavior `" + behavior.id + "`");
        if (behavior.behavior.empty()) {
            context.report("TESSERA_IR_MALFORMED",
                           "behavior `" + behavior.id + "` names no engine behavior",
                           behavior.source);
        }
        for (const auto& target : behavior.targets) {
            if (target.rfind("ref::", 0) != 0) {
                context.report("TESSERA_IR_MALFORMED",
                               "behavior `" + behavior.id + "` target `" + target +
                                   "` is not a ref",
                               behavior.source);
            }
        }
        for (std::size_t index = 0; index < behavior.args.size(); ++index) {
            for (std::size_t other = index + 1; other < behavior.args.size(); ++other) {
                if (behavior.args[index].name == behavior.args[other].name) {
                    context.report("TESSERA_IR_MALFORMED",
                                   "behavior `" + behavior.id + "` declares arg `" +
                                       behavior.args[index].name + "` twice",
                                   behavior.source);
                }
            }
        }
        for (const auto& output : behavior.outputs) {
            validate_type(context, output.type, behavior.source);
            scope.declared_types.emplace(behavior.id + "." + output.name, output.type);
        }
    }
    // ADR-050 section 3.1 (AF5): a service requirement is an `svc::<name>` id with the C++ spelling
    // of its interface. The type and header are the project manifest's data, so they are required
    // here: without them codegen would have to guess a C++ name.
    for (const auto& service : component.services) {
        declare(service.id, "svc::", service.source);
        validate_span(context, service.source, "service `" + service.id + "`");
        if (service.name.empty()) {
            context.report("TESSERA_IR_MALFORMED", "service `" + service.id + "` has no name",
                           service.source);
        }
        if (service.type.empty()) {
            context.report("TESSERA_IR_MALFORMED", "service `" + service.id + "` has no type",
                           service.source);
        }
        if (service.header.empty()) {
            context.report("TESSERA_IR_MALFORMED", "service `" + service.id + "` has no header",
                           service.source);
        }
    }
    // ADR-067 item 1: a resource is a `res::<name>` id; its status/data/error are the dotted refs.
    // The entry's own checks (service, key, coverage) run after the node tree, in
    // `validate_resources`.
    for (const auto& resource : component.resources) {
        declare(resource.id, "res::", resource.source);
        scope.declared_types.emplace(resource.id + ".status", "string");
        if (!resource.type.empty()) {
            scope.declared_types.emplace(resource.id + ".data", resource.type);
        }
    }
    for (const auto& ref : component.refs) {
        declare(ref.id, "ref::", where);
    }
}

void ComponentChecker::check_handlers() {
    for (const auto& handler : component.handlers) {
        if (!starts_with(handler.id, "handler::") || handler.id.size() == 9) {
            context.report("TESSERA_IR_MALFORMED",
                           "handler id `" + handler.id + "` must be handler::N", handler.source);
        }
        if (!scope.handlers.insert(handler.id).second) {
            context.report("TESSERA_IR_DUPLICATE_ID", "duplicate handler id `" + handler.id + "`",
                           handler.source);
        }
        validate_span(context, handler.source, "handler `" + handler.id + "`");
    }
    // Actions are checked after all handler ids exist, so source order does not matter.
    for (const auto& handler : component.handlers) {
        LocalNames locals;
        for (const auto& param : handler.params) {
            locals.push_back(param.name);
            validate_type(context, param.type, handler.source);
        }
        validate_actions(context, scope, locals, handler.actions, handler.source);
    }
}

bool ComponentChecker::resolve_binding(const std::string& id, const std::string& what) {
    const auto found = context.bindings.find(id);
    if (found == context.bindings.end() || found->second->component != component.id) {
        context.report("TESSERA_IR_UNRESOLVED", what + " names unknown binding `" + id + "`",
                       component.source);
        return false;
    }
    context.used_bindings.insert(id);
    return true;
}

// Section 6: an effect body and its cleanup are protected regions, so their bindings have kind
// `effect`; a native event handler binding has kind `event` (checked on the node that uses it).
bool ComponentChecker::resolve_effect_binding(const std::string& id, const std::string& what) {
    if (!resolve_binding(id, what)) {
        return false;
    }
    const auto found = context.bindings.find(id);
    if (found != context.bindings.end() && found->second->kind != BindingKind::Effect) {
        context.report("TESSERA_IR_BINDING",
                       what + " names `" + id + "`, which is not an effect binding (section 6)",
                       component.source);
    }
    return true;
}

void ComponentChecker::check_effects_and_refs() {
    for (const auto& effect : component.effects) {
        declare(effect.id, "effect::", effect.source);
        validate_span(context, effect.source, "effect `" + effect.id + "`");
        for (const auto& dep : effect.deps) {
            if (!reference_resolves(scope, {}, dep) || starts_with(dep, "ref::") ||
                starts_with(dep, "env::")) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "effect `" + effect.id + "` depends on `" + dep +
                                   "`, which is not a prop, state variable or animated value",
                               effect.source);
            }
        }
        resolve_effect_binding(effect.handler, "effect `" + effect.id + "`");
        if (!effect.cleanup.empty()) {
            resolve_effect_binding(effect.cleanup, "effect `" + effect.id + "` cleanup");
        }
    }
    for (const auto& ref : component.refs) {
        if (scope.node_ids.count(ref.target) == 0) {
            context.report("TESSERA_IR_UNRESOLVED",
                           "ref `" + ref.id + "` targets unknown node `" + ref.target + "`",
                           component.source);
        }
    }
}

} // namespace

ComponentScope make_scope(const Component& component) {
    ComponentScope scope;
    scope.component = &component;
    for (const auto& prop : component.props) {
        scope.props.insert(prop.name);
        scope.declared_types.emplace("prop::" + prop.name, prop.type);
        if (prop.name == "children" && prop.type == "children") {
            scope.declares_children = true;
        }
    }
    for (const auto& state : component.state) {
        scope.ids.insert(state.id);
        scope.declared_types.emplace(state.id, state.type);
    }
    for (const auto& value : component.animated_values) {
        scope.ids.insert(value.id);
        scope.declared_types.emplace(value.id, value.type);
    }
    for (const auto& env : component.environment) {
        scope.ids.insert(env.id);
        scope.declared_types.emplace(env.id, env.type);
    }
    for (const auto& ref : component.refs) {
        scope.ids.insert(ref.id);
    }
    for (const auto& resource : component.resources) {
        scope.ids.insert(resource.id);
        scope.declared_types.emplace(resource.id + ".status", "string");
        if (!resource.type.empty()) {
            scope.declared_types.emplace(resource.id + ".data", resource.type);
        }
    }
    for (const auto& handler : component.handlers) {
        scope.handlers.insert(handler.id);
    }
    return scope;
}

void validate_component(ValidationContext& context, const Component& component) {
    ComponentChecker checker{context, component, make_scope(component)};
    // `make_scope` pre-fills the ids and handlers so a preview body or an effect dependency can
    // resolve one; the checker's `declare` pass rebuilds both sets itself, so that a duplicate
    // declaration is reported once per offending entry rather than once per pre-filled id.
    checker.scope.ids.clear();
    checker.scope.handlers.clear();
    if (component.name.empty()) {
        context.report("TESSERA_IR_MALFORMED", "a component has no name", component.source);
    }
    if (component.component_id.empty()) {
        context.report("TESSERA_IR_MISSING_ID",
                       "component `" + component.id + "` has no componentId", component.source);
    } else if (!context.component_numeric_ids.insert(component.component_id).second) {
        context.report("TESSERA_IR_DUPLICATE_ID",
                       "componentId `" + component.component_id + "` is used twice",
                       component.source);
    }
    validate_span(context, component.source, "component `" + component.id + "`");

    // Declarations first: later passes resolve references against them.
    checker.check_declarations();
    checker.check_handlers();
    // ADR-066 item 5 (issue #802): a provider node's value has the type of the context it
    // provides, so a record-typed context makes the position a record-value position. The
    // component's `environment[]` entries carry the types (the environment is the consumer's IR
    // form), so the map is built here, where they are declared.
    std::map<std::string, std::string> provider_types;
    for (const auto& env : component.environment) {
        if (env.provider.rfind("context:", 0) == 0) {
            provider_types.emplace(env.provider.substr(8), env.type);
        }
    }
    validate_node_tree(context, checker.scope, component.root, provider_types);
    checker.check_effects_and_refs();
    validate_resources(context, checker.scope, component);
}

} // namespace tessera::compiler::ir
