// SPDX-License-Identifier: MIT

// `useResource(service.method, key, { suspend })` and `useDeferredValue(x)` (ADR-067 items 1 and 5,
// docs/specs/pending-ui-v1.md sections 3 and 6, issue #656). The form is checked when the hook
// pass meets the statement, so the symbol exists for the order checks; the service and method are
// resolved after `check_services`, which declares the `useService` bindings they name.

#include "expr_walk.hpp"
#include "reporter.hpp"

#include <string>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;
using jsx::Statement;
using jsx::StatementKind;

constexpr std::string_view kHookArg = "TESSERA_REACT_HOOK_ARG";
constexpr std::string_view kHookForm = "TESSERA_REACT_HOOK_FORM";
constexpr std::string_view kMethod = "TESSERA_REACT_SERVICE_METHOD";

constexpr std::string_view kResourceShape =
    "write `const feed = useResource(api.loadFeed, { query }, { suspend: true });`";

// A key is evaluated every frame to compare it by value, so it must be pure: no call, no function,
// no JSX (pending-ui-v1.md section 3.1).
const Expr* impure_part(const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Call:
    case ExprKind::Arrow:
    case ExprKind::Jsx:
    case ExprKind::Block:
    case ExprKind::If:
    case ExprKind::Switch:
    case ExprKind::Const:
    case ExprKind::Return:
        return &expr;
    default:
        break;
    }
    for (const auto& operand : expr.operands) {
        if (operand) {
            if (const Expr* found = impure_part(*operand)) {
                return found;
            }
        }
    }
    for (const auto& property : expr.properties) {
        if (property.value) {
            if (const Expr* found = impure_part(*property.value)) {
                return found;
            }
        }
    }
    return nullptr;
}

std::string params_text(const ResourceInfo& info) {
    std::string text;
    for (const auto& [name, type] : info.params) {
        text += (text.empty() ? "" : ", ") + name + ": " + type;
    }
    return text.empty() ? "no parameters" : text;
}

// The key is the argument list of the fetch (pending-ui-v1.md section 3.1): one scalar for a method
// of one parameter, or a record whose fields are the method's parameter names, once each. The key
// part types are checked in lowering, where the cell types are known.
bool key_matches_params(Context& ctx, const ResourceInfo& info, const std::string& service) {
    const std::string where =
        "`" + service + "." + info.method + "` takes (" + params_text(info) + ")";
    const Expr& key = *info.key;
    if (key.kind != ExprKind::Object) {
        if (info.params.size() != 1) {
            ctx.error(kHookArg,
                      "the key of `useResource` is one scalar but " + where +
                          "; write a record key whose fields are the parameter names",
                      key.span,
                      "write `{ " +
                          (info.params.empty() ? std::string("...") : info.params[0].first) +
                          " }`");
            return false;
        }
        return true;
    }
    bool ok = true;
    for (const jsx::ObjectProperty& property : key.properties) {
        bool known = false;
        for (const auto& param : info.params) {
            known = known || param.first == property.key;
        }
        if (!known) {
            ctx.error(kHookArg, "`" + property.key + "` is not a parameter of the method: " + where,
                      property.key_span, "name the key fields after the method's parameters");
            ok = false;
        }
    }
    for (const auto& param : info.params) {
        std::size_t seen = 0;
        for (const jsx::ObjectProperty& property : key.properties) {
            seen += property.key == param.first ? 1 : 0;
        }
        if (seen != 1) {
            ctx.error(kHookArg,
                      std::string(seen == 0 ? "the key has no field for parameter `"
                                            : "the key repeats parameter `") +
                          param.first + "`: " + where,
                      key.span, "give each parameter exactly one key field");
            ok = false;
        }
    }
    return ok;
}

} // namespace

bool is_resource_call(const Expr& call) {
    return callee_name(call) == "useResource";
}

void check_resource(Context& ctx, std::size_t index, const Statement& statement, const Expr& call) {
    ResourceInfo info;
    info.statement = index;
    info.span = statement.span;
    const bool bound = statement.kind == StatementKind::Const;
    if (bound) {
        if (statement.array_pattern || statement.names.size() != 1) {
            ctx.error(kHookForm, "`useResource` is not written in its accepted form",
                      statement.span, std::string(kResourceShape));
            return;
        }
        info.name = statement.names.front();
        add_symbol(ctx, info.name, SymbolKind::Resource, statement.span, index);
    } else if (statement.kind != StatementKind::Expression) {
        ctx.error(kHookForm, "`useResource` is not written in its accepted form", statement.span,
                  std::string(kResourceShape));
        return;
    }
    // operand 0 is the callee: the service method, the key and an optional options object follow.
    if (call.operands.size() < 3 || call.operands.size() > 4) {
        ctx.error(kHookArg, "`useResource` takes a service method, a key and optional options",
                  call.span, std::string(kResourceShape));
        return;
    }
    const Expr& method = *call.operands[1];
    if (method.kind != ExprKind::Member || method.operands.size() != 1 || !method.operands[0] ||
        method.operands[0]->kind != ExprKind::Identifier || method.text == "[]") {
        ctx.error(kHookArg,
                  "the first argument of `useResource` is a method of a `useService` binding, "
                  "not a URL or a function",
                  method.span, std::string(kResourceShape));
        return;
    }
    info.service_binding = method.operands[0]->text;
    info.method = method.text;
    const Expr& key = *call.operands[2];
    if (const Expr* impure = impure_part(key)) {
        ctx.error(kHookArg, "a resource key is a pure expression of state, props or environment",
                  impure->span, "compute the key from values, not from calls");
        return;
    }
    info.key = &key;
    if (call.operands.size() == 4) {
        const Expr& options = *call.operands[3];
        if (options.kind != ExprKind::Object || options.spread_base != nullptr) {
            ctx.error(kHookArg, "the options of `useResource` are one object literal", options.span,
                      "write `{ suspend: true }`");
            return;
        }
        for (const jsx::ObjectProperty& property : options.properties) {
            if (property.key != "suspend") {
                ctx.error(kHookArg,
                          "`" + property.key +
                              "` is not an option of `useResource`; V1 has only `suspend`",
                          property.key_span, "write `{ suspend: true }`");
                return;
            }
            if (!property.value || property.value->kind != ExprKind::Boolean) {
                ctx.error(kHookArg, "`suspend` is the literal `true` or `false`",
                          property.value ? property.value->span : property.key_span);
                return;
            }
            info.suspend = property.value->boolean;
        }
    }
    ctx.out.resources.push_back(std::move(info));
}

void check_resource_services(Context& ctx) {
    auto& resources = ctx.out.resources;
    for (std::size_t at = 0; at < resources.size();) {
        ResourceInfo& info = resources[at];
        const Symbol* binding = ctx.out.find(info.service_binding);
        const ServiceUseInfo* use = nullptr;
        if (binding != nullptr && binding->kind == SymbolKind::Service) {
            for (const ServiceUseInfo& candidate : ctx.out.service_uses) {
                if (candidate.name == binding->name) {
                    use = &candidate;
                    break;
                }
            }
        }
        if (use == nullptr || use->decl == nullptr) {
            ctx.error(kHookArg,
                      "`" + info.service_binding +
                          "` is not a `useService` binding; `useResource` reads a service method",
                      info.span, "declare `const api = useService<Api>();` above it");
            resources.erase(resources.begin() + static_cast<std::ptrdiff_t>(at));
            continue;
        }
        if (use->statement >= info.statement) {
            ctx.error("TESSERA_REACT_HOOK_ORDER",
                      "`" + info.service_binding + "` is used before the hook that declares it",
                      info.span, "move the `useService` above this statement");
            resources.erase(resources.begin() + static_cast<std::ptrdiff_t>(at));
            continue;
        }
        const services::ManifestMethod* method = use->decl->find_method(info.method);
        if (method == nullptr) {
            ctx.error(kMethod,
                      "service `" + use->service + "` lists no method `" + info.method + "`",
                      info.span, "the method is declared in `ui/services.json`");
            resources.erase(resources.begin() + static_cast<std::ptrdiff_t>(at));
            continue;
        }
        const std::string& returns = method->returns;
        if (returns == "int" || returns == "float" || returns == "bool" || returns == "string") {
            info.type = returns;
        }
        for (const services::ManifestParam& param : method->params) {
            info.params.emplace_back(param.name, param.type);
        }
        if (!key_matches_params(ctx, info, use->service)) {
            resources.erase(resources.begin() + static_cast<std::ptrdiff_t>(at));
            continue;
        }
        ++at;
    }
}

void check_deferred_value(Context& ctx, std::size_t index, const Statement& statement,
                          const Expr& call, const jsx::HookEntry& hook) {
    const bool ok = statement.kind == StatementKind::Const && !statement.array_pattern &&
                    statement.names.size() == 1 && call.operands.size() == 2 && call.operands[1] &&
                    call.operands[1]->kind == ExprKind::Identifier;
    if (!ok) {
        ctx.error(kHookForm, "`useDeferredValue` is not written in its accepted form",
                  statement.span, "write `const settled = useDeferredValue(query);`");
        return;
    }
    const Symbol* source = ctx.out.find(call.operands[1]->text);
    if (source == nullptr ||
        (source->kind != SymbolKind::State && source->kind != SymbolKind::Prop)) {
        ctx.error(
            kHookArg,
            "`useDeferredValue` takes a `useState` value or a destructured prop of the component",
            call.operands[1]->span, "pass the state variable or the prop by name");
        return;
    }
    // Copied first: `add_symbol` may reallocate the table `source` points into.
    const std::string source_name = source->name;
    const bool source_is_prop = source->kind == SymbolKind::Prop;
    add_symbol(ctx, statement.names.front(), SymbolKind::DeferredValue, statement.span, index);
    BehaviorInfo info;
    info.statement = index;
    info.name = statement.names.front();
    info.value_name = statement.names.front();
    info.hook = hook.name;
    info.behavior = hook.provider;
    info.source_state = source_name;
    info.source_is_prop = source_is_prop;
    for (const jsx::HookOutput& output : hook.outputs) {
        info.outputs.push_back(output.name);
        info.output_types.push_back(output.ir);
    }
    ctx.out.behaviors.push_back(std::move(info));
}

} // namespace tessera::compiler::rules
