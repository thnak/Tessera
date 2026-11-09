// SPDX-License-Identifier: MIT

// Bindings of slice 6 (docs/specs/ir-v1.md section 6): `useEffect` statements and declared arrows
// annotated with `// Note: bind::<id>`. Both create protected regions, so both produce a Binding;
// an un-annotated setter-call arrow produces an inline handler record instead (section 2.1).

#include "hook_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

std::string prefixed(std::string_view prefix, const std::string& name) {
    return std::string(prefix) + name;
}

std::optional<std::string> dependency_id(const rules::ComponentAnalysis& analysis,
                                         const std::string& name) {
    const rules::Symbol* symbol = analysis.find(name);
    if (symbol == nullptr) {
        return std::nullopt;
    }
    switch (symbol->kind) {
    case rules::SymbolKind::Prop:
        return prefixed("prop::", name);
    case rules::SymbolKind::State:
        return prefixed("state::", name);
    case rules::SymbolKind::AnimatedValue:
        return prefixed("anim::", name);
    default:
        return std::nullopt;
    }
}

} // namespace

void lower_effect_hook(HookState& state, std::size_t index, LoweredHooks& out) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::EffectInfo* info = nullptr;
    for (const rules::EffectInfo& candidate : context.analysis.effects) {
        if (candidate.statement == index) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr || info->body == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "the analysis did not classify this `useEffect`",
                  statement.span);
        return;
    }
    // Every effect body is developer code in a protected region (docs/specs/ir-v1.md section 2
    // requires `handler` to be a binding id), and the only stable region id comes from the
    // annotation. A generated position-based id would break across edits (decision 5).
    if (statement.bind_id.empty()) {
        fail_hook(state, "TESSERA_IR_BINDING",
                  "a `useEffect` needs a `// Note: bind::<id>` annotation: its body is a protected "
                  "region in generated C++",
                  statement.span, "add `// Note: bind::<id>` on the line before it");
        return;
    }
    ir::Effect effect;
    effect.id = prefixed("effect::", statement.bind_id);
    effect.handler = prefixed("bind::", statement.bind_id);
    effect.source = to_source_span(context.source_map, statement.span, context.file);
    for (const std::string& dependency : info->dependencies) {
        const std::optional<std::string> id = dependency_id(context.analysis, dependency);
        if (!id) {
            fail_hook(state, "TESSERA_IR_UNRESOLVED",
                      "effect dependency `" + dependency +
                          "` is not a prop, state variable or animated value",
                      statement.span);
            return;
        }
        effect.deps.push_back(*id);
    }
    ir::Binding binding;
    binding.id = effect.handler;
    binding.component = out.component_id;
    binding.kind = ir::BindingKind::Effect;
    binding.source = to_source_span(context.source_map, statement.bind_span, context.file);
    binding.region = statement.bind_id;
    if (info->cleanup != nullptr) {
        effect.cleanup = prefixed("bind::", statement.bind_id + "_cleanup");
        ir::Binding cleanup;
        cleanup.id = effect.cleanup;
        cleanup.component = out.component_id;
        cleanup.kind = ir::BindingKind::Effect;
        cleanup.source = to_source_span(context.source_map, info->cleanup->span, context.file);
        cleanup.region = statement.bind_id + "_cleanup";
        out.bindings.push_back(std::move(cleanup));
    }
    out.bindings.push_back(std::move(binding));
    out.effects.push_back(std::move(effect));
}

void lower_declared_arrow_hook(HookState& state, std::size_t index, LoweredHooks& out) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const jsx::Expr& arrow = *statement.init;
    const rules::ArrowInfo* info = context.analysis.find_arrow(&arrow);
    if (info == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "the analysis did not classify this handler arrow",
                  arrow.span);
        return;
    }
    if (statement.names.empty()) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "a handler declaration has no name",
                  statement.span);
        return;
    }
    std::vector<ir::Param> params = lower_handler_params(state, arrow);
    if (state.error) {
        return;
    }
    if (!statement.bind_id.empty()) {
        const std::optional<std::string> return_type = lower_return_type(arrow.return_type);
        if (!return_type) {
            fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                      "the handler return type has no IR form in v1", arrow.span);
            return;
        }
        ir::Binding binding;
        binding.id = prefixed("bind::", statement.bind_id);
        binding.component = out.component_id;
        binding.kind = ir::BindingKind::Event;
        binding.source = to_source_span(context.source_map, statement.bind_span, context.file);
        binding.region = statement.bind_id;
        binding.params = std::move(params);
        binding.return_type = *return_type;
        out.bindings.push_back(std::move(binding));
        const HandlerRef ref{HandlerRefKind::Binding, out.bindings.back().id};
        for (const std::string& name : statement.names) {
            out.handler_index.push_back({name, ref});
        }
        return;
    }
    if (info->role != rules::ArrowRole::InlineHandler) {
        // A declared arrow that is neither annotated nor setter-only is BIND_MISSING in analysis,
        // so reaching this point means the analysis and the AST disagree.
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "a declared arrow is neither a native nor an inline handler", arrow.span);
        return;
    }
    std::vector<ir::Action> actions = lower_actions(state, arrow);
    if (state.error) {
        return;
    }
    const std::string id =
        out.add_handler(std::move(params), std::move(actions),
                        to_source_span(context.source_map, arrow.span, context.file));
    for (const std::string& name : statement.names) {
        out.handler_index.push_back({name, HandlerRef{HandlerRefKind::InlineHandler, id}});
    }
}

} // namespace tessera::compiler::lowering::internal
