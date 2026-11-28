// SPDX-License-Identifier: MIT

// `useObservable(svc.member)` (ADR-050 open question 6, docs/design/observable-render-read.md):
// the analyzer checked the form and resolved the service and the member against the manifest, so
// this turns the recorded statement into an IR `observables[]` entry.

#include "hook_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

namespace tessera::compiler::lowering::internal {

void lower_observable_hook(HookState& state, std::size_t index, LoweredHooks& out) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::ObservableInfo* info = nullptr;
    for (const rules::ObservableInfo& candidate : context.analysis.observables) {
        if (candidate.statement == index) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr || info->type.empty()) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "the analysis did not classify this `useObservable` call", statement.span);
        return;
    }
    ir::Observable observable;
    observable.id = "obs::" + info->name;
    observable.service = "svc::" + info->service_binding;
    observable.member = info->member;
    observable.type = info->type;
    observable.source = to_source_span(context.source_map, statement.span, context.file);
    out.observables.push_back(std::move(observable));
}

} // namespace tessera::compiler::lowering::internal
