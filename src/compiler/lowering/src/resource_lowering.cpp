// SPDX-License-Identifier: MIT

// `useResource` and `useDeferredValue` (ADR-067 items 1 and 5, docs/specs/pending-ui-v1.md
// sections 3.4 and 6, issue #656). The analyzer recorded the accepted forms and resolved the
// service; this turns them into the IR `resources[]` entry and the `deferred_value` behavior.

#include "hook_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

// A `useState` value the deferred value may hold: a bool, int or float, or a string that the
// declaration types as a literal union (an enum-like value). A plain `string` has no fixed size
// in V1's allocation-free update, so it is rejected (pending-ui-v1.md section 6, decision 5).
bool deferrable(const jsx::Statement& declaration, const std::string& ir_type) {
    if (ir_type == "bool" || ir_type == "int" || ir_type == "float") {
        return true;
    }
    if (ir_type != "string" || !declaration.init || declaration.init->type_args.size() != 1 ||
        !declaration.init->type_args[0]) {
        return false;
    }
    const jsx::TypeNode& type = *declaration.init->type_args[0];
    if (type.kind == jsx::TypeKind::StringLiteral) {
        return true;
    }
    if (type.kind != jsx::TypeKind::Union || type.children.empty()) {
        return false;
    }
    for (const jsx::TypePtr& child : type.children) {
        if (!child || child->kind != jsx::TypeKind::StringLiteral) {
            return false;
        }
    }
    return true;
}

} // namespace

void lower_resource_hook(HookState& state, std::size_t index, LoweredHooks& out,
                         const NameResolver& names) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::ResourceInfo* info = nullptr;
    for (const rules::ResourceInfo& candidate : context.analysis.resources) {
        if (candidate.statement == index) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr || info->key == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "the analysis did not classify this `useResource` call", statement.span);
        return;
    }
    ir::Resource resource;
    resource.id = "res::" + (info->name.empty()
                                 ? info->method + "_" + std::to_string(out.resources.size() + 1)
                                 : info->name);
    resource.service = "svc::" + info->service_binding;
    resource.method = info->method;
    resource.suspend = info->suspend;
    resource.type = info->type;
    resource.source = to_source_span(context.source_map, statement.span, context.file);
    const jsx::Expr& key = *info->key;
    if (key.kind == jsx::ExprKind::Object) {
        // `{ query }` is an anonymous record of scalars; the validator checks each part.
        std::vector<ir::RecordField> fields;
        for (const jsx::ObjectProperty& property : key.properties) {
            if (!property.value) {
                fail_hook(state, "TESSERA_IR_MALFORMED", "a key field has no value", key.span);
                return;
            }
            ir::ExprPtr value = lower_component_expression(state, *property.value, names);
            if (!value) {
                return;
            }
            fields.push_back(ir::RecordField{property.key, std::move(value)});
        }
        if (key.spread_base != nullptr) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED", "a resource key does not take a spread",
                      key.span);
            return;
        }
        resource.key = std::move(*ir::make_record("", std::move(fields)));
    } else {
        ir::ExprPtr value = lower_component_expression(state, key, names);
        if (!value) {
            return;
        }
        resource.key = std::move(*value);
    }
    out.resources.push_back(std::move(resource));
}

void lower_deferred_value_hook(HookState& state, std::size_t index, LoweredHooks& out,
                               const NameResolver& names) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::BehaviorInfo* info = nullptr;
    for (const rules::BehaviorInfo& candidate : context.analysis.behaviors) {
        if (candidate.statement == index) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr || info->source_state.empty()) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "the analysis did not classify this `useDeferredValue` call", statement.span);
        return;
    }
    const std::string source_id = "state::" + info->source_state;
    const ir::State* source = nullptr;
    for (const ir::State& candidate : out.state) {
        if (candidate.id == source_id) {
            source = &candidate;
            break;
        }
    }
    const rules::Symbol* symbol = context.analysis.find(info->source_state);
    if (source == nullptr || symbol == nullptr ||
        symbol->statement >= context.component.body.size() ||
        !deferrable(context.component.body[symbol->statement], source->type)) {
        fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                  "`useDeferredValue` holds a bool, number or enum-like (string literal union) "
                  "state value; `" +
                      info->source_state + "` is none of those",
                  statement.span, "defer a scalar and derive lists or records from it");
        return;
    }
    ir::ExprPtr value =
        lower_component_expression(state, *context.component.body[index].init->operands[1], names);
    if (!value) {
        return;
    }
    ir::Behavior behavior;
    behavior.id = "beh::" + info->name;
    behavior.hook = info->hook;
    behavior.behavior = info->behavior;
    behavior.source = to_source_span(context.source_map, statement.span, context.file);
    behavior.outputs.push_back(ir::BehaviorOutput{info->outputs.front(), source->type});
    ir::BehaviorArg arg;
    arg.name = "value";
    arg.expr = std::move(*value);
    behavior.args.push_back(std::move(arg));
    out.behaviors.push_back(std::move(behavior));
}

} // namespace tessera::compiler::lowering::internal
