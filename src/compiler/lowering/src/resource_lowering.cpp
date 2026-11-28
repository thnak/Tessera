// SPDX-License-Identifier: MIT

// `useResource` and `useDeferredValue` (ADR-067 items 1 and 5, docs/specs/pending-ui-v1.md
// sections 3.4 and 6, issue #656). The analyzer recorded the accepted forms and resolved the
// service; this turns them into the IR `resources[]` entry and the `deferred_value` behavior.

#include "hook_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
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

// The IR type of a key part when it is a literal or a state or prop cell; empty when it is any
// other expression (an arithmetic result, a derived value), which the check then lets through.
std::string key_part_type(const HookState& state, const LoweredHooks& out, const ir::Expr& part) {
    if (part.kind == ir::ExprKind::Literal) {
        if (std::holds_alternative<bool>(part.literal)) {
            return "bool";
        }
        if (std::holds_alternative<std::int64_t>(part.literal)) {
            return "int";
        }
        if (std::holds_alternative<double>(part.literal)) {
            return "float";
        }
        return std::holds_alternative<std::string>(part.literal) ? "string" : "";
    }
    if (part.kind != ir::ExprKind::Ref) {
        return {};
    }
    for (const ir::State& cell : out.state) {
        if (cell.id == part.id) {
            return cell.type;
        }
    }
    if (part.id.rfind("prop::", 0) == 0 && state.context.declared_props != nullptr) {
        for (const ir::Prop& prop : *state.context.declared_props) {
            if ("prop::" + prop.name == part.id) {
                return prop.type;
            }
        }
    }
    return {};
}

// A key part supplies a parameter when the types are equal, or an int supplies a float.
bool key_type_fits(const std::string& supplied, const std::string& declared) {
    return supplied.empty() || supplied == declared || (supplied == "int" && declared == "float");
}

// pending-ui-v1.md section 3.1: the key parts must have the types of the method's parameters. The
// shape (which fields, how many) was checked by the analyzer against the same parameter list.
bool check_key_types(HookState& state, const LoweredHooks& out,
                     const std::vector<std::pair<std::string, std::string>>& params,
                     const ir::Expr& key, jsx::Span span) {
    const auto check = [&](const std::string& name, const std::string& declared,
                           const ir::Expr& part) {
        const std::string supplied = key_part_type(state, out, part);
        if (key_type_fits(supplied, declared)) {
            return true;
        }
        fail_hook(state, "TESSERA_REACT_HOOK_ARG",
                  "the key part for parameter `" + name + "` has type `" + supplied +
                      "`, but the service method declares `" + declared + "`",
                  span, "pass a value of type `" + declared + "`");
        return false;
    };
    if (key.kind != ir::ExprKind::Record) {
        return params.size() != 1 || check(params[0].first, params[0].second, key);
    }
    for (const ir::RecordField& field : key.fields) {
        for (const auto& [name, declared] : params) {
            if (name == field.name && field.value && !check(name, declared, *field.value)) {
                return false;
            }
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
    if (!check_key_types(state, out, info->params, resource.key, key.span)) {
        return;
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
    // The deferred source: a `useState` cell, or a destructured prop (pending-ui-v1.md section 6).
    // A prop's IR type is `string` for a literal union too, so the lowering of the props type
    // records which string props are one (#960); a plain `string` prop is rejected like a state.
    std::string source_type;
    if (info->source_is_prop) {
        if (context.declared_props != nullptr) {
            for (const ir::Prop& prop : *context.declared_props) {
                if (prop.name == info->source_state) {
                    source_type = prop.type;
                }
            }
        }
        bool enum_like = false;
        if (source_type == "string" && context.enum_like_props != nullptr) {
            enum_like = std::find(context.enum_like_props->begin(), context.enum_like_props->end(),
                                  info->source_state) != context.enum_like_props->end();
        }
        if (source_type != "bool" && source_type != "int" && source_type != "float" && !enum_like) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                      "`useDeferredValue` holds a bool, number or enum-like (string literal "
                      "union) prop; `" +
                          info->source_state + "` is none of those",
                      statement.span,
                      "defer a bool or number prop, type the string as a literal union, or copy "
                      "the prop into `useState`");
            return;
        }
    } else {
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
        source_type = source->type;
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
    behavior.outputs.push_back(ir::BehaviorOutput{info->outputs.front(), source_type});
    ir::BehaviorArg arg;
    arg.name = "value";
    arg.expr = std::move(*value);
    behavior.args.push_back(std::move(arg));
    out.behaviors.push_back(std::move(behavior));
}

} // namespace tessera::compiler::lowering::internal
