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

void check_animated(Context& ctx, std::size_t index, const Statement& statement,
                    const Expr& call) {
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
    } else {
        report_unknown_hook(ctx, name, call.operands[0]->span);
    }
}

} // namespace

void report_unknown_hook(Context& ctx, std::string_view name, jsx::Span span) {
    const bool unsupported = name == "useMemo" || name == "useCallback" || name == "useReducer" ||
                             name == "useContext";
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
        if (statement.init && is_hook_name(callee_name(*statement.init))) {
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
