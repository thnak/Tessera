// SPDX-License-Identifier: MIT

// Symbol table and hook forms (docs/specs/jsx-subset-v1.md section 7). Nesting and declaration
// order are in hook_placement.cpp.

#include "expr_walk.hpp"
#include "reporter.hpp"

namespace tessera::compiler::rules {

void add_symbol(Context& ctx, std::string name, SymbolKind kind, jsx::Span span,
                std::size_t statement) {
    ctx.out.symbols.push_back({std::move(name), kind, span, statement});
}

namespace {

using jsx::Expr;
using jsx::ExprKind;
using jsx::Statement;
using jsx::StatementKind;

// The `options` parameter of a behavior hook, or nullptr when the hook takes none. The manifest is
// the authority on the option list, so the analyzer asks it rather than keeping a second copy.
const jsx::HookParam* find_options_param(const jsx::HookEntry& hook) {
    for (const jsx::HookParam& param : hook.params) {
        if (param.kind == "options") {
            return &param;
        }
    }
    return nullptr;
}

// One declared option field by name, or nullptr when the manifest does not declare it.
const jsx::HookParam* find_option_field(const jsx::HookParam& options, std::string_view name) {
    for (const jsx::HookParam& field : options.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

void register_props(Context& ctx) {
    const jsx::ModuleItem& item = ctx.item;
    if (item.has_props && !item.props_destructured && !item.props_name.empty()) {
        add_symbol(ctx, item.props_name, SymbolKind::PropsParam, item.name_span, kNoStatement);
    }
    for (const auto& binding : item.props_bindings) {
        add_symbol(ctx, binding.name, SymbolKind::Prop, binding.span, kNoStatement);
    }
}

void form_error(Context& ctx, std::string_view hook, const Statement& statement,
                std::string_view form) {
    ctx.error("TESSERA_REACT_HOOK_FORM",
              "`" + std::string(hook) + "` is not written in its accepted form", statement.span,
              "write `" + std::string(form) + "`");
}

void check_state(Context& ctx, std::size_t index, const Statement& statement, const Expr& call) {
    for (std::size_t i = 0; i < statement.names.size() && i < 2; ++i) {
        add_symbol(ctx, statement.names[i], i == 0 ? SymbolKind::State : SymbolKind::StateSetter,
                   statement.span, index);
    }
    const bool ok = statement.kind == StatementKind::Const && statement.array_pattern &&
                    statement.names.size() == 2 && call.operands.size() == 2;
    if (!ok) {
        form_error(ctx, "useState", statement, "const [x, setX] = useState<T>(init);");
        return;
    }
    // ADR-058 typing rule 3: an empty `[]` or `{}` initial carries no type, and TypeScript would
    // infer `never[]` / `{}` here. Say so with the one-line fix instead of the lowering's generic
    // "needs a type argument".
    const Expr& initial = *call.operands[1];
    if (call.type_args.empty() && initial.kind == ExprKind::Array && initial.operands.empty()) {
        ctx.error("TESSERA_JSX_TYPE_STATE_UNTYPED", "an empty array initial has no element type",
                  initial.span, "write `useState<Item[]>([])`, naming the element type");
    } else if (call.type_args.empty() && initial.kind == ExprKind::Object &&
               initial.properties.empty() && initial.spread_base == nullptr) {
        ctx.error("TESSERA_JSX_TYPE_STATE_UNTYPED", "an empty object initial has no type",
                  initial.span, "write `useState<Item>({})`, naming a record type");
    }
}

void check_ref(Context& ctx, std::size_t index, const Statement& statement, const Expr& call) {
    if (!statement.names.empty()) {
        add_symbol(ctx, statement.names[0], SymbolKind::Ref, statement.span, index);
    }
    const bool ok = statement.kind == StatementKind::Const && !statement.array_pattern &&
                    statement.names.size() == 1 && call.operands.size() == 2 &&
                    call.operands[1]->kind == ExprKind::Null;
    if (!ok) {
        form_error(ctx, "useRef", statement, "const r = useRef<Element>(null);");
    }
}

// `useMergedRefs` (ADR-064 item 7, issue #630): one ref that fills several owners. The binding is a
// ref, so it is declared as one; the operands must all be refs, which is checked here rather than
// in the lowering so the developer sees the error at the call site.
void check_merged_refs(Context& ctx, std::size_t index, const Statement& statement,
                       const Expr& call) {
    if (!statement.names.empty()) {
        add_symbol(ctx, statement.names[0], SymbolKind::Ref, statement.span, index);
    }
    const bool ok = statement.kind == StatementKind::Const && !statement.array_pattern &&
                    statement.names.size() == 1 && call.operands.size() >= 3;
    if (!ok) {
        form_error(ctx, "useMergedRefs", statement, "const r = useMergedRefs(a, b);");
        return;
    }
    for (std::size_t operand = 1; operand < call.operands.size(); ++operand) {
        const Expr* ref = call.operands[operand].get();
        if (ref == nullptr || ref->kind != ExprKind::Identifier) {
            form_error(ctx, "useMergedRefs", statement, "const r = useMergedRefs(a, b);");
            return;
        }
        const Symbol* symbol = ctx.out.find(ref->text);
        if (symbol == nullptr || symbol->kind != SymbolKind::Ref) {
            form_error(ctx, "useMergedRefs", statement,
                       "`useMergedRefs` takes refs declared by `useRef`");
            return;
        }
    }
}

void check_animated(Context& ctx, std::size_t index, const Statement& statement, const Expr& call) {
    if (!statement.names.empty()) {
        add_symbol(ctx, statement.names[0], SymbolKind::AnimatedValue, statement.span, index);
    }
    const bool ok = statement.kind == StatementKind::Const && !statement.array_pattern &&
                    statement.names.size() == 1 && call.operands.size() == 2;
    if (!ok) {
        form_error(ctx, "useAnimatedValue", statement, "const v = useAnimatedValue(init);");
    }
}

void check_effect(Context& ctx, std::size_t index, const Statement& statement, const Expr& call) {
    const bool shape = statement.kind == StatementKind::Expression && call.operands.size() >= 2 &&
                       call.operands.size() <= 3 && call.operands[1]->kind == ExprKind::Arrow &&
                       call.operands[1]->params.empty();
    if (!shape) {
        form_error(ctx, "useEffect", statement, "useEffect(() => { ... }, [dep, ...]);");
        return;
    }
    EffectInfo effect;
    effect.statement = index;
    effect.body = call.operands[1].get();
    effect.bind_id = statement.bind_id;
    effect.dependency_array = call.operands.size() == 3 ? call.operands[2].get() : nullptr;
    ctx.out.effects.push_back(std::move(effect));
}

// A behavior hook call (ADR-064 item 7; docs/specs/behavior-hooks-v1.md section 2):
//
//   const trap = useFocusTrap(trapRef, { active: open, restoreFocus: true, onEscape: () => ... });
//
// The call is one `const` binding (or a bare statement), the leading arguments are `useRef` names
// in manifest order, and the last argument is the options object literal. The manifest is the
// authority on how many targets and which fields, so the check compares against it rather than
// re-deriving the shape; a hook the manifest does not declare is `TESSERA_REACT_UNKNOWN_HOOK` as
// for any other.
void check_behavior(Context& ctx, std::size_t index, const Statement& statement, const Expr& call,
                    const jsx::HookEntry& hook) {
    BehaviorInfo info;
    info.statement = index;
    info.hook = hook.name;
    info.behavior = hook.provider;
    info.exclusive = hook.exclusive;
    if (statement.kind == StatementKind::Const && !statement.array_pattern) {
        if (!statement.names.empty()) {
            info.name = statement.names.front();
        }
    } else if (statement.kind != StatementKind::Expression) {
        form_error(ctx, hook.name, statement,
                   "const " + std::string(info.name.empty() ? "name" : info.name) + " = " +
                       hook.name + "(target, { ... });");
        return;
    }

    // The leading arguments are the target refs, in manifest order. A `ref` parameter is the only
    // kind that consumes one; everything else is the options object.
    std::size_t operand = 1; // operand 0 is the callee
    for (const jsx::HookParam& param : hook.params) {
        if (param.kind != "ref") {
            continue;
        }
        if (operand >= call.operands.size()) {
            form_error(ctx, hook.name, statement,
                       hook.name + " takes a target ref per target the manifest lists");
            return;
        }
        const Expr& target = *call.operands[operand];
        if (target.kind != ExprKind::Identifier) {
            form_error(ctx, hook.name, statement, "a target is a `useRef` name");
            return;
        }
        info.targets.push_back(target.text);
        ++operand;
    }
    if (operand < call.operands.size()) {
        const Expr& options_expr = *call.operands[operand];
        if (options_expr.kind != ExprKind::Object) {
            form_error(ctx, hook.name, statement, "the options are one object literal");
            return;
        }
        info.options = &options_expr;
        ++operand;
    }
    if (operand != call.operands.size()) {
        form_error(ctx, hook.name, statement, "the call takes a target and one options object");
        return;
    }
    // Every written option must be one the manifest declares, and every option the manifest
    // declares without a default must be written. An unknown key is a typo the user would otherwise
    // never see: the behavior would silently read its default.
    if (const jsx::HookParam* options_param = find_options_param(hook); options_param != nullptr) {
        if (info.options != nullptr) {
            for (const jsx::ObjectProperty& property : info.options->properties) {
                if (find_option_field(*options_param, property.key) == nullptr) {
                    form_error(ctx, hook.name, statement,
                               "`" + property.key + "` is not an option of " + hook.name);
                    return;
                }
            }
        }
        for (const jsx::HookParam& field : options_param->fields) {
            if (!field.default_text.empty()) {
                continue;
            }
            bool written = false;
            if (info.options != nullptr) {
                for (const jsx::ObjectProperty& property : info.options->properties) {
                    written = written || property.key == field.name;
                }
            }
            if (!written) {
                form_error(ctx, hook.name, statement,
                           "`" + field.name + "` is required by " + hook.name);
                return;
            }
        }
    }
    for (const jsx::HookOutput& output : hook.outputs) {
        info.outputs.push_back(output.name);
        info.output_types.push_back(output.ir);
    }
    ctx.out.behaviors.push_back(std::move(info));
}

// An `environment` hook call (docs/specs/jsx-subset-v1.md section 7.1): an engine-owned value the
// component reads, never a state cell it owns.
//
//   const bp = useBreakpoint();
//   const theme = useTheme();
//   const [theme, setTheme] = useThemeState();
//
// The manifest is the authority on the return type and the provider, so the check compares against
// it. A read-only hook takes no arguments and binds one name; a writable one binds the
// `[value, setter]` pair. A bare statement is a form error: an environment value nobody reads is
// dead code, and the IR has no id for it.
void check_environment(Context& ctx, std::size_t index, const Statement& statement,
                       const Expr& call, const jsx::HookEntry& hook) {
    EnvironmentInfo info;
    info.statement = index;
    info.hook = hook.name;
    info.provider = hook.provider;
    info.type = hook.returns_ir;
    info.writable = hook.writable;
    if (info.type.empty()) {
        // The manifest declares a record return, which no environment hook has today. Rejecting it
        // here keeps the IR's `environment[]` a list of scalar values.
        form_error(ctx, hook.name, statement, "const value = " + hook.name + "();");
        return;
    }
    // A parameterized environment hook (`useMediaQuery("(min-width: 600px)")`) takes exactly one
    // argument, which the manifest declares as a `value` parameter. A hook with no such parameter
    // takes none, so the arity is the manifest's answer rather than a name check.
    std::size_t expected_args = 1; // operand 0 is the callee
    for (const jsx::HookParam& param : hook.params) {
        if (param.kind == "value") {
            ++expected_args;
        }
    }
    const bool shape =
        statement.kind == StatementKind::Const && call.operands.size() == expected_args;
    if (!shape) {
        form_error(ctx, hook.name, statement,
                   hook.writable ? "const [value, setValue] = " + hook.name + "();"
                                 : "const value = " + hook.name + "();");
        return;
    }
    if (expected_args > 1) {
        info.argument = call.operands[1].get();
    }
    if (hook.writable) {
        if (!statement.array_pattern || statement.names.size() != 2) {
            form_error(ctx, hook.name, statement, "const [value, setValue] = " + hook.name + "();");
            return;
        }
        info.name = statement.names[0];
        info.setter = statement.names[1];
        add_symbol(ctx, info.name, SymbolKind::Environment, statement.span, index);
        add_symbol(ctx, info.setter, SymbolKind::StateSetter, statement.span, index);
    } else {
        if (statement.array_pattern || statement.names.size() != 1) {
            form_error(ctx, hook.name, statement, "const value = " + hook.name + "();");
            return;
        }
        info.name = statement.names[0];
        add_symbol(ctx, info.name, SymbolKind::Environment, statement.span, index);
    }
    ctx.out.environment.push_back(std::move(info));
}

// `useTransition` (ADR-067 item 4, docs/specs/pending-ui-v1.md section 5): the pair
// `[isPending, startTransition]`. The hook has no arguments and no target. It is recorded as a
// behavior of the `transition` engine behavior whose one output is `isPending`; the second name is
// a function value whose calls the arrow rules (arrows.cpp) and the lowering understand.
void check_transition(Context& ctx, std::size_t index, const Statement& statement, const Expr& call,
                      const jsx::HookEntry& hook) {
    const bool ok = statement.kind == StatementKind::Const && statement.array_pattern &&
                    statement.names.size() == 2 && call.operands.size() == 1;
    if (!ok) {
        form_error(ctx, hook.name, statement,
                   "const [isPending, startTransition] = useTransition();");
        return;
    }
    add_symbol(ctx, statement.names[0], SymbolKind::TransitionPending, statement.span, index);
    add_symbol(ctx, statement.names[1], SymbolKind::TransitionStart, statement.span, index);
    BehaviorInfo info;
    info.statement = index;
    info.name = statement.names[0];
    info.value_name = statement.names[0];
    info.hook = hook.name;
    info.behavior = hook.provider;
    for (const jsx::HookOutput& output : hook.outputs) {
        info.outputs.push_back(output.name);
        info.output_types.push_back(output.ir);
    }
    ctx.out.behaviors.push_back(std::move(info));
}

// `useId` (ADR-066 item 1): a stable id from the instance key and the call ordinal. It takes no
// arguments and binds one name, which is a string the component can use for an `id`/`aria-*`
// association. The value is engine-owned like an environment hook, so it is recorded as one: the
// generated base computes it once from the instance identity, and nothing in the component writes
// it.
void check_id(Context& ctx, std::size_t index, const Statement& statement, const Expr& call) {
    if (!statement.names.empty()) {
        add_symbol(ctx, statement.names[0], SymbolKind::Environment, statement.span, index);
    }
    const bool ok = statement.kind == StatementKind::Const && !statement.array_pattern &&
                    statement.names.size() == 1 && call.operands.size() == 1;
    if (!ok) {
        form_error(ctx, "useId", statement, "const id = useId();");
        return;
    }
    EnvironmentInfo info;
    info.statement = index;
    info.hook = "useId";
    info.provider = "id";
    info.type = "string";
    info.name = statement.names.front();
    ctx.out.environment.push_back(std::move(info));
}

// `useLayoutEffect` (ADR-066 item 1): accepted with `useEffect` timing, because there is no browser
// paint to order against. The form and the dependency rules are `useEffect`'s, so the same check
// runs and the effect is recorded identically.
void check_layout_effect(Context& ctx, std::size_t index, const Statement& statement,
                         const Expr& call) {
    check_effect(ctx, index, statement, call);
}

void check_hook_statement(Context& ctx, std::size_t index, const Statement& statement) {
    const Expr& call = *statement.init;
    const std::string_view name = callee_name(call);
    if (name == "useState") {
        check_state(ctx, index, statement, call);
    } else if (name == "useReducer") {
        // ADR-066 item 4: handled by its own pass (reducer.cpp), which needs the module's helper
        // and type aliases. Nothing is recorded here, so a rejected form reports once.
        return;
    } else if (name == "useContext") {
        // ADR-066 item 5: handled by its own pass (context.cpp), which needs the module's
        // `createContext` consts. Nothing is recorded here, so a rejected form reports once.
        return;
    } else if (name == "useService") {
        // ADR-050 section 3.1: handled by its own pass (service.cpp), which needs the project's
        // services manifest. Nothing is recorded here, so a rejected form reports once.
        return;
    } else if (name == "useRef") {
        check_ref(ctx, index, statement, call);
    } else if (name == "useMergedRefs") {
        check_merged_refs(ctx, index, statement, call);
    } else if (name == "useAnimatedValue") {
        check_animated(ctx, index, statement, call);
    } else if (name == "useEffect") {
        check_effect(ctx, index, statement, call);
    } else if (name == "useLayoutEffect") {
        check_layout_effect(ctx, index, statement, call);
    } else if (const jsx::HookEntry* transition = ctx.manifest.find(name);
               name == "useTransition" && transition != nullptr) {
        check_transition(ctx, index, statement, call, *transition);
    } else if (name == "useResource") {
        check_resource(ctx, index, statement, call);
    } else if (name == "useObservable") {
        check_observable(ctx, index, statement, call);
    } else if (const jsx::HookEntry* deferred = ctx.manifest.find(name);
               name == "useDeferredValue" && deferred != nullptr) {
        check_deferred_value(ctx, index, statement, call, *deferred);
    } else if (name == "useId") {
        check_id(ctx, index, statement, call);
    } else if (const jsx::HookEntry* behavior = ctx.manifest.find(name);
               behavior != nullptr && behavior->lowering == jsx::HookLoweringKind::Behavior) {
        check_behavior(ctx, index, statement, call, *behavior);
    } else if (const jsx::HookEntry* environment = ctx.manifest.find(name);
               environment != nullptr &&
               environment->lowering == jsx::HookLoweringKind::Environment) {
        check_environment(ctx, index, statement, call, *environment);
    } else {
        report_unknown_hook(ctx, name, call.operands[0]->span);
    }
}

} // namespace

void report_unknown_hook(Context& ctx, std::string_view name, jsx::Span span) {
    // `useContext` is ADR-066 item 5 (its own pass, context.cpp); `useMemo`/`useCallback` stay
    // unsupported: a `const` of a pure expression covers the memo case, and a callback is a
    // handler.
    const bool unsupported = name == "useMemo" || name == "useCallback";
    if (unsupported) {
        ctx.error("TESSERA_REACT_UNSUPPORTED_API",
                  "`" + std::string(name) + "` is not supported in V1", span,
                  "compute the value in a `const`; state updates are batched already");
        return;
    }
    ctx.error("TESSERA_REACT_UNKNOWN_HOOK",
              "unknown hook `" + std::string(name) +
                  "`: only useState, useReducer, useEffect, useRef, useAnimatedValue, useId and "
                  "useLayoutEffect are available",
              span, "use a core hook, or write a native handler");
}

void check_hooks(Context& ctx) {
    register_props(ctx);
    const auto& body = ctx.item.body;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const Statement& statement = body[i];
        if (statement.init && is_hook_name(callee_name(*statement.init)) &&
            !is_expansion_name(callee_name(*statement.init))) {
            check_hook_statement(ctx, i, statement);
            continue;
        }
        if (statement.kind == StatementKind::Const) {
            const bool handler = statement.init && statement.init->kind == ExprKind::Arrow;
            for (const auto& name : statement.names) {
                add_symbol(ctx, name, handler ? SymbolKind::Handler : SymbolKind::Local,
                           statement.span, i);
            }
        }
    }
    // ADR-066 item 4 runs before the placement rules so that its two symbols exist for the order
    // check, and before the arrow-role classification, which decides whether a body of
    // `dispatch({...})` calls is an inline handler.
    check_reducers(ctx);
    check_hook_placement(ctx);
}

} // namespace tessera::compiler::rules
