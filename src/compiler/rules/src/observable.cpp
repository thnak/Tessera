// SPDX-License-Identifier: MIT

// `useObservable(svc.member)` (ADR-050 open question 6, docs/design/observable-render-read.md): a
// render-time read of a service-owned `Observable<T>` listed in the services manifest.
//
//   const nav = useService<Navigator>();
//   const route = useObservable(nav.route);     // top level of the component body
//   <Text>{route.title}</Text>                   // read-only; a record is read by field
//
// The form is checked when the hook pass meets the statement, so the symbol exists for the order
// checks; the service and member are resolved after `check_services`, which declares the bindings.

#include "expr_walk.hpp"
#include "reporter.hpp"

#include <string>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;

constexpr std::string_view kHookArg = "TESSERA_REACT_HOOK_ARG";
constexpr std::string_view kHookForm = "TESSERA_REACT_HOOK_FORM";
constexpr std::string_view kShape = "write `const route = useObservable(nav.route);`";

std::string observable_names(const services::ManifestService& service) {
    std::string names;
    for (const services::ManifestObservable& observable : service.observables) {
        names += (names.empty() ? "" : ", ") + observable.name;
    }
    return names.empty() ? "none" : names;
}

} // namespace

bool is_observable_call(const Expr& call) {
    return callee_name(call) == "useObservable";
}

void check_observable(Context& ctx, std::size_t index, const jsx::Statement& statement,
                      const Expr& call) {
    const bool ok = statement.kind == jsx::StatementKind::Const && !statement.array_pattern &&
                    statement.names.size() == 1 && call.operands.size() == 2 &&
                    call.type_args.empty();
    if (!ok) {
        ctx.error(kHookForm, "`useObservable` is not written in its accepted form", statement.span,
                  std::string(kShape));
        return;
    }
    const Expr& source = *call.operands[1];
    if (source.kind != ExprKind::Member || source.operands.size() != 1 || !source.operands[0] ||
        source.operands[0]->kind != ExprKind::Identifier || source.text == "[]") {
        ctx.error(kHookArg,
                  "the argument of `useObservable` is a member of a `useService` binding, not a "
                  "variable, a call or a conditional",
                  source.span, std::string(kShape));
        return;
    }
    ObservableInfo info;
    info.statement = index;
    info.span = statement.span;
    info.name = statement.names.front();
    info.service_binding = source.operands[0]->text;
    info.member = source.text;
    add_symbol(ctx, info.name, SymbolKind::Observable, statement.span, index);
    ctx.out.observables.push_back(std::move(info));
}

void check_observable_services(Context& ctx) {
    auto& observables = ctx.out.observables;
    for (std::size_t at = 0; at < observables.size();) {
        ObservableInfo& info = observables[at];
        const ServiceUseInfo* use = nullptr;
        if (const Symbol* binding = ctx.out.find(info.service_binding);
            binding != nullptr && binding->kind == SymbolKind::Service) {
            for (const ServiceUseInfo& candidate : ctx.out.service_uses) {
                if (candidate.name == binding->name) {
                    use = &candidate;
                    break;
                }
            }
        }
        const auto drop = [&]() {
            observables.erase(observables.begin() + static_cast<std::ptrdiff_t>(at));
        };
        if (use == nullptr || use->decl == nullptr) {
            ctx.error(kHookArg,
                      "`" + info.service_binding +
                          "` is not a `useService` binding; `useObservable` reads a member of one",
                      info.span, "declare `const nav = useService<Navigator>();` above it");
            drop();
            continue;
        }
        if (use->statement >= info.statement) {
            ctx.error("TESSERA_REACT_HOOK_ORDER",
                      "`" + info.service_binding + "` is used before the hook that declares it",
                      info.span, "move the `useService` above this statement");
            drop();
            continue;
        }
        const services::ManifestObservable* member = use->decl->find_observable(info.member);
        if (member == nullptr) {
            ctx.error(kHookArg,
                      "service `" + use->service + "` lists no observable `" + info.member + "`",
                      info.span,
                      "the manifest lists: " + observable_names(*use->decl) +
                          " (declare it under `observables` in `ui/services.json`)");
            drop();
            continue;
        }
        info.type = member->type;
        ++at;
    }
}

} // namespace tessera::compiler::rules
