// SPDX-License-Identifier: MIT

// Bindings of slice 6 (docs/specs/ir-v1.md section 6): `useEffect` statements and declared arrows
// annotated with `// Note: bind::<id>`. Both create protected regions, so both produce a Binding;
// an un-annotated setter-call arrow produces an inline handler record instead (section 2.1).

#include "expression_lowering_internal.hpp"
#include "hook_lowering_internal.hpp"
#include "record_internal.hpp"
#include "tessera/compiler/jsx/hook_manifest.hpp"
#include "tessera/compiler/lowering/source_span.hpp"
#include "tessera/compiler/rules/analyze.hpp"

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

// A manifest default as the IR literal it denotes. The manifest writes the default as text
// (`true`, `false`, a number, or a closed token), so the IR type decides how to read it: a `bool`
// field's default is `true`/`false`, a `number` field's is a number, and anything else is a string
// token.
ir::ExprPtr default_literal(const jsx::HookParam& field) {
    ir::ExprPtr expr = std::make_unique<ir::Expr>();
    if (field.ir == "bool") {
        expr->kind = ir::ExprKind::Literal;
        expr->literal = field.default_text == "true";
        return expr;
    }
    if (field.ir == "number" || field.ir == "int" || field.ir == "float") {
        expr->kind = ir::ExprKind::Literal;
        expr->literal = std::stod(field.default_text);
        return expr;
    }
    expr->kind = ir::ExprKind::Literal;
    expr->literal = field.default_text;
    return expr;
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
        binding.preview = lower_preview(state, arrow);
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
    if (const std::optional<jsx::Diagnostic> violation =
            check_callback_calls(context, params, actions, arrow.span)) {
        state.error = violation;
        return;
    }
    const std::string id =
        out.add_handler(std::move(params), std::move(actions),
                        to_source_span(context.source_map, arrow.span, context.file));
    for (const std::string& name : statement.names) {
        out.handler_index.push_back({name, HandlerRef{HandlerRefKind::InlineHandler, id}});
    }
}

// One behavior hook call (ADR-064 item 7; docs/specs/behavior-hooks-v1.md section 6). The analyzer
// already checked the form against the manifest, so this only turns the recorded call into IR: the
// id, the targets, one arg per manifest field in manifest order, and the outputs.
void lower_behavior_hook(HookState& state, std::size_t index, LoweredHooks& out,
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
    if (info == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED", "the analysis did not classify this behavior call",
                  statement.span);
        return;
    }
    ir::Behavior behavior;
    // A bare statement has no local name, so it is numbered from 1 in declaration order, which is
    // the rule the spec states for `beh::<hook>_<n>`.
    std::string name = info->name;
    if (name.empty()) {
        name = info->hook + "_" + std::to_string(out.behaviors.size() + 1);
    }
    behavior.id = prefixed("beh::", name);
    behavior.hook = info->hook;
    behavior.behavior = info->behavior;
    behavior.exclusive = info->exclusive;
    behavior.source = to_source_span(context.source_map, statement.span, context.file);
    for (const std::string& target : info->targets) {
        behavior.targets.push_back(prefixed("ref::", target));
    }
    for (std::size_t output_index = 0; output_index < info->outputs.size(); ++output_index) {
        const std::string type = output_index < info->output_types.size()
                                     ? info->output_types[output_index]
                                     : std::string("bool");
        behavior.outputs.push_back(ir::BehaviorOutput{info->outputs[output_index], type});
    }
    // The options, in manifest order, with every defaulted option written out as a literal. The
    // spec's rule (section 6) is that a consumer needs no manifest to evaluate a behavior, so the
    // IR carries the full arg list rather than only what the source wrote.
    if (const jsx::HookEntry* hook = rules::builtin_hook_manifest().find(info->hook);
        hook != nullptr) {
        for (const jsx::HookParam& param : hook->params) {
            if (param.kind != "options") {
                continue;
            }
            for (const jsx::HookParam& field : param.fields) {
                ir::BehaviorArg arg;
                arg.name = field.name;
                const jsx::ObjectProperty* written = nullptr;
                if (info->options != nullptr) {
                    for (const jsx::ObjectProperty& property : info->options->properties) {
                        if (property.key == field.name) {
                            written = &property;
                            break;
                        }
                    }
                }
                if (written != nullptr && written->value != nullptr) {
                    ir::ExprPtr lowered = lower_component_expression(state, *written->value, names);
                    if (!lowered) {
                        return;
                    }
                    arg.expr = std::move(*lowered);
                } else if (!field.default_text.empty()) {
                    // The manifest's default, as the literal the IR would carry.
                    ir::ExprPtr fallback = default_literal(field);
                    arg.expr = std::move(*fallback);
                }
                behavior.args.push_back(std::move(arg));
            }
        }
    }
    out.behaviors.push_back(std::move(behavior));
}

// One `environment` hook call (docs/specs/jsx-subset-v1.md section 7.1). The analyzer already
// checked the form against the manifest, so this only turns the recorded call into IR: the id, the
// provider, the type and whether the value is writable.
void lower_environment_hook(HookState& state, std::size_t index, LoweredHooks& out,
                            const NameResolver& names) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::EnvironmentInfo* info = nullptr;
    for (const rules::EnvironmentInfo& candidate : context.analysis.environment) {
        if (candidate.statement == index) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "the analysis did not classify this environment call", statement.span);
        return;
    }
    ir::Environment env;
    env.id = prefixed("env::", info->name);
    env.name = info->name;
    env.provider = info->provider;
    env.type = info->type;
    env.writable = info->writable;
    env.source = to_source_span(context.source_map, statement.span, context.file);
    // A parameterized environment hook (`useMediaQuery`) carries its one argument, so the generated
    // base can evaluate it without the manifest.
    if (info->argument != nullptr) {
        ir::ExprPtr lowered = lower_component_expression(state, *info->argument, names);
        if (!lowered) {
            return;
        }
        env.argument = std::move(*lowered);
    }
    out.environment.push_back(std::move(env));
}

// ADR-066 item 5 (issue #802): one `const theme = useContext(ThemeContext);` call. The IR carries
// it as an environment entry whose provider is `context:<CtxName>` — a namespace the engine
// providers never use — so the generated base can tell a compile-time context value from a
// host-pushed one. The type is the context's declared one; the default is the `createContext`
// value, which the consumer with no ancestor provider falls back to (the owner's decision).
void lower_context_use(HookState& state, std::size_t index, LoweredHooks& out,
                       const NameResolver& names) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::ContextUseInfo* info = nullptr;
    for (const rules::ContextUseInfo& candidate : context.analysis.context_uses) {
        if (candidate.statement == index) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr || context.analyzed == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "the analysis did not classify this `useContext` call", statement.span);
        return;
    }
    const rules::ContextInfo* declared = context.analyzed->find_context(info->context);
    if (declared == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "`" + info->context + "` is not a context of this module", statement.span);
        return;
    }
    ir::Environment env;
    env.id = prefixed("env::", info->name);
    env.name = info->name;
    env.provider = "context:" + declared->name;
    env.type = declared->ir_type;
    env.writable = false;
    env.source = to_source_span(context.source_map, statement.span, context.file);
    // The `createContext` default: the value a consumer reads when no ancestor provider resolves.
    // A record-typed default is an object literal that lowers to an `ir::Record` — the same rule a
    // prop default follows (ADR-051 slice 3) — and a primitive default is an ordinary expression.
    if (declared->default_value != nullptr) {
        const ir::RecordType* record = nullptr;
        if (context.records != nullptr) {
            if (const std::optional<std::string_view> name =
                    record_name_of(*context.records, declared->ir_type)) {
                record = find_record(*context.records, *name);
            }
        }
        if (record != nullptr) {
            if (declared->default_value->kind != jsx::ExprKind::Object) {
                fail_hook(state, "TESSERA_IR_RECORD_VALUE",
                          "the default of `" + declared->name + "` is type `" + record->name +
                              "`, so it must be an object literal",
                          declared->span);
                return;
            }
            RecordLoweringState record_state{context.source_map, context.file, std::nullopt};
            ir::ExprPtr lowered = lower_record_value(record_state, *declared->default_value,
                                                     *record, *context.records);
            if (!lowered) {
                if (!state.error && record_state.error) {
                    state.error = std::move(record_state.error);
                }
                if (!state.error) {
                    fail_hook(state, "TESSERA_IR_RECORD_VALUE",
                              "the default of `" + declared->name + "` does not lower",
                              declared->span);
                }
                return;
            }
            env.argument = std::move(*lowered);
        } else {
            ir::ExprPtr lowered =
                lower_component_expression(state, *declared->default_value, names);
            if (!lowered) {
                return;
            }
            env.argument = std::move(*lowered);
        }
    }
    out.environment.push_back(std::move(env));
}

void lower_service_hook(HookState& state, std::size_t index, LoweredHooks& out) {
    const HookLoweringContext& context = state.context;
    const jsx::Statement& statement = context.component.body[index];
    const rules::ServiceUseInfo* info = nullptr;
    for (const rules::ServiceUseInfo& candidate : context.analysis.service_uses) {
        if (candidate.statement == index) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr || info->decl == nullptr) {
        fail_hook(state, "TESSERA_IR_MALFORMED",
                  "the analysis did not classify this `useService` call", statement.span);
        return;
    }
    ir::Service service;
    service.id = prefixed("svc::", info->name);
    service.name = info->name;
    service.type = info->decl->cpp_type;
    service.header = info->decl->header;
    service.registry_id = info->decl->id;
    service.source = to_source_span(context.source_map, statement.span, context.file);
    out.services.push_back(std::move(service));
}

} // namespace tessera::compiler::lowering::internal
