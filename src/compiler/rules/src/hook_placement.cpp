// SPDX-License-Identifier: MIT

// Where hooks may appear (docs/specs/jsx-subset-v1.md section 7): only as top-level statements of
// the component body, and before any statement that reads what they declare.

#include "expr_walk.hpp"
#include "reporter.hpp"

#include <algorithm>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;

enum class Where { Expression, Condition, Function };

std::string_view where_text(Where where) {
    switch (where) {
    case Where::Condition:
        return "inside a condition";
    case Where::Function:
        return "inside a function (handler, effect or callback)";
    case Where::Expression:
        break;
    }
    return "inside an expression";
}

// Finds every hook call except the statement-level `top` call, which hooks.cpp checks.
class NestedHookScanner {
public:
    NestedHookScanner(Context& ctx, const Expr* top)
        : ctx_(ctx),
          top_(top) {}

    void scan(const Expr& expr, Where where) {
        if (&expr != top_ && is_hook_name(callee_name(expr))) {
            report(expr, where);
        }
        switch (expr.kind) {
        case ExprKind::Arrow:
            scan_all(expr, Where::Function);
            return;
        case ExprKind::Conditional:
            scan(*expr.operands[0], where);
            scan(*expr.operands[1], Where::Condition);
            scan(*expr.operands[2], Where::Condition);
            return;
        case ExprKind::Binary:
            if (expr.text == "&&" || expr.text == "||") {
                scan(*expr.operands[0], where);
                scan(*expr.operands[1], Where::Condition);
                return;
            }
            break;
        default:
            break;
        }
        scan_all(expr, where);
    }

    void scan_jsx(const jsx::JsxNode& node) {
        auto visit = [this](const Expr& child) { scan(child, Where::Expression); };
        for_each_jsx_expr(node, visit);
    }

private:
    void scan_all(const Expr& expr, Where where) {
        auto visit = [this, where](const Expr& child) { scan(child, where); };
        for_each_child(expr, visit);
    }

    void report(const Expr& call, Where where) {
        const std::string name(callee_name(call));
        if (!is_core_hook(name)) {
            report_unknown_hook(ctx_, name, call.operands[0]->span);
            return;
        }
        ctx_.error("TESSERA_REACT_HOOK_ORDER",
                   "hook `" + name + "` is called " + std::string(where_text(where)) +
                       "; hooks are only allowed at the top level of the component body",
                   call.span, "move the call to the top of the component");
    }

    Context& ctx_;
    const Expr* top_;
};

bool is_hook_symbol(SymbolKind kind) {
    return kind == SymbolKind::State || kind == SymbolKind::StateSetter ||
           kind == SymbolKind::Ref || kind == SymbolKind::AnimatedValue;
}

void check_nesting(Context& ctx) {
    for (const auto& statement : ctx.item.body) {
        if (!statement.init) {
            continue;
        }
        const Expr* top = is_hook_name(callee_name(*statement.init)) ? statement.init.get() : nullptr;
        NestedHookScanner scanner(ctx, top);
        if (top != nullptr) {
            // The call itself is fine; its arguments are not allowed to hold hooks.
            for (std::size_t i = 1; i < top->operands.size(); ++i) {
                scanner.scan(*top->operands[i], Where::Expression);
            }
        } else {
            scanner.scan(*statement.init, Where::Expression);
        }
    }
    if (ctx.item.returned) {
        NestedHookScanner scanner(ctx, nullptr);
        scanner.scan_jsx(*ctx.item.returned);
    }
}

// A statement may not read a name declared by a hook further down: the hooks run first and keep a
// fixed order, so a later hook cannot feed an earlier statement.
void check_order(Context& ctx) {
    const auto& body = ctx.item.body;
    ReadOptions options;
    options.include_callees = true;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (!body[i].init) {
            continue;
        }
        std::vector<std::string> reported;
        for (const auto& read : collect_reads(*body[i].init, options)) {
            const Symbol* symbol = ctx.out.find(read.name);
            const bool later = symbol != nullptr && is_hook_symbol(symbol->kind) &&
                               symbol->statement != kNoStatement && symbol->statement > i;
            if (!later || std::find(reported.begin(), reported.end(), read.name) != reported.end()) {
                continue;
            }
            reported.push_back(read.name);
            ctx.error("TESSERA_REACT_HOOK_ORDER",
                      "`" + read.name + "` is used before the hook that declares it",
                      read.span, "move the hook above this statement");
        }
    }
}

} // namespace

void check_hook_placement(Context& ctx) {
    check_nesting(ctx);
    check_order(ctx);
}

} // namespace tessera::compiler::rules
