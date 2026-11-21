// SPDX-License-Identifier: MIT

// Symbol table and hook forms (docs/specs/jsx-subset-v1.md section 7). Nesting and declaration
// order are in hook_placement.cpp.

#include "expr_walk.hpp"
#include "reporter.hpp"

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;
using jsx::Statement;
using jsx::StatementKind;

void add_symbol(Context& ctx, std::string name, SymbolKind kind, jsx::Span span,
                std::size_t statement) {
    ctx.out.symbols.push_back({std::move(name), kind, span, statement});
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
    for (const jsx::HookOutput& output : hook.outputs) {
        info.outputs.push_back(output.name);
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
    const bool shape = statement.kind == StatementKind::Const && call.operands.size() == 1;
    if (!shape) {
        form_error(ctx, hook.name, statement,
                   hook.writable ? "const [value, setValue] = " + hook.name + "();"
                                 : "const value = " + hook.name + "();");
        return;
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

void check_hook_statement(Context& ctx, std::size_t index, const Statement& statement) {
    const Expr& call = *statement.init;
    const std::string_view name = callee_name(call);
    if (name == "useState") {
        check_state(ctx, index, statement, call);
    } else if (name == "useRef") {
        check_ref(ctx, index, statement, call);
    } else if (name == "useAnimatedValue") {
        check_animated(ctx, index, statement, call);
    } else if (name == "useEffect") {
        check_effect(ctx, index, statement, call);
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
    const bool unsupported =
        name == "useMemo" || name == "useCallback" || name == "useReducer" || name == "useContext";
    if (unsupported) {
        ctx.error("TESSERA_REACT_UNSUPPORTED_API",
                  "`" + std::string(name) + "` is not supported in V1", span,
                  "compute the value in a `const`; state updates are batched already");
        return;
    }
    ctx.error("TESSERA_REACT_UNKNOWN_HOOK",
              "unknown hook `" + std::string(name) +
                  "`: only useState, useEffect, useRef and useAnimatedValue are available",
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
    check_hook_placement(ctx);
}

} // namespace tessera::compiler::rules
