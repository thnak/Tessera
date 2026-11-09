// SPDX-License-Identifier: MIT

// Module-level checks (docs/specs/ir-v1.md sections 1, 6, 9 and 11).

#include "validate_internal.hpp"

#include <string_view>
#include <utility>

namespace tessera::compiler::ir {
namespace {

constexpr std::string_view kBindPrefix = "bind::";

void check_bindings(ValidationContext& context) {
    for (std::size_t i = 0; i < context.module.bindings.size(); ++i) {
        const Binding& binding = context.module.bindings[i];
        const bool prefixed =
            binding.id.rfind(kBindPrefix, 0) == 0 && binding.id.size() > kBindPrefix.size();
        if (!prefixed) {
            context.report("TESSERA_IR_BINDING",
                           "binding id `" + binding.id + "` must be bind::<id>", binding.source);
        } else if (binding.region != binding.id.substr(kBindPrefix.size())) {
            context.report("TESSERA_IR_BINDING",
                           "binding `" + binding.id + "` has region `" + binding.region +
                               "`, expected the id without `bind::`",
                           binding.source);
        }
        const auto first = context.bindings.find(binding.id);
        if (first != context.bindings.end() && first->second != &binding) {
            context.report("TESSERA_IR_DUPLICATE_ID", "duplicate binding id `" + binding.id + "`",
                           binding.source);
        }
        validate_span(context, binding.source, "binding `" + binding.id + "`");
        if (i > 0 && !(context.module.bindings[i - 1].id < binding.id)) {
            context.report("TESSERA_IR_ORDER", "bindings must be sorted by id", binding.source);
        }
        for (const auto& param : binding.params) {
            validate_type(context, param.type, binding.source);
        }
        if (!binding.return_type.empty()) {
            validate_type(context, binding.return_type, binding.source);
        }
        if (context.used_bindings.count(binding.id) == 0) {
            context.report("TESSERA_IR_BINDING",
                           "binding `" + binding.id + "` is not used by any event or effect",
                           binding.source);
        }
    }
}

void check_previews(ValidationContext& context) {
    for (const auto& binding : context.module.bindings) {
        if (!binding.preview) {
            continue;
        }
        const Component* owner = nullptr;
        for (const auto& component : context.module.components) {
            if (component.id == binding.component) {
                owner = &component;
                break;
            }
        }
        if (owner == nullptr) {
            context.report("TESSERA_IR_UNRESOLVED",
                           "binding `" + binding.id + "` names unknown component `" +
                               binding.component + "`",
                           binding.source);
            continue;
        }
        const ComponentScope scope = make_scope(*owner);
        LocalNames locals;
        for (const auto& param : binding.params) {
            locals.push_back(param.name);
        }
        validate_actions(context, scope, locals, binding.preview->actions, binding.source);
        validate_expr(context, scope, locals, binding.preview->return_value.get(), binding.source);
    }
}

} // namespace

std::vector<IrDiagnostic> validate(const IrModule& module) {
    ValidationContext context{module, {}, {}, {}, {}, {}, {}};
    const SourceSpan none;
    if (module.ir_version != kIrVersion) {
        context.report("TESSERA_IR_VERSION",
                       "unsupported irVersion " + std::to_string(module.ir_version), none);
    }
    for (const auto& component : module.components) {
        if (!context.component_ids.insert(component.id).second) {
            context.report("TESSERA_IR_DUPLICATE_ID",
                           "duplicate component id `" + component.id + "`", component.source);
        }
    }
    for (const auto& binding : module.bindings) {
        // The first one wins; later ones are reported as duplicates.
        context.bindings.emplace(binding.id, &binding);
    }
    for (std::size_t i = 0; i < module.components.size(); ++i) {
        const Component& component = module.components[i];
        if (i > 0) {
            const Component& before = module.components[i - 1];
            const bool ordered =
                before.source.file < component.source.file ||
                (before.source.file == component.source.file && before.name < component.name);
            if (!ordered) {
                context.report("TESSERA_IR_ORDER",
                               "components must be sorted by source file, then name",
                               component.source);
            }
        }
        validate_component(context, component);
    }
    check_bindings(context);
    check_previews(context);
    return std::move(context.diagnostics);
}

} // namespace tessera::compiler::ir
