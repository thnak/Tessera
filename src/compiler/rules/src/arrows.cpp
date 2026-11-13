// SPDX-License-Identifier: MIT

// Arrow roles (docs/specs/jsx-subset-v1.md section 9, docs/design/jsx-ir-lowering.md decision 4)
// and `return` placement in effect bodies (section 7).

#include "expr_walk.hpp"
#include "reporter.hpp"

#include <algorithm>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;
using jsx::StatementKind;

void add_arrow(Context& ctx, const Expr& arrow, ArrowRole role, std::string bind_id = {}) {
    if (ctx.out.find_arrow(&arrow) == nullptr) {
        ctx.out.arrows.push_back({&arrow, role, std::move(bind_id)});
    }
}

bool is_setter_call(const Context& ctx, const Expr& expr) {
    // `guard && call(...)` (issue #236): the guard is a pure expression, the call is checked below.
    if (expr.kind == ExprKind::Binary && expr.text == "&&" && expr.operands.size() == 2) {
        return expr.operands[1]->kind == ExprKind::Call && is_setter_call(ctx, *expr.operands[1]);
    }
    if (expr.kind != ExprKind::Call || expr.operands.empty()) {
        return false;
    }
    const Expr& callee = *expr.operands[0];
    if (callee.kind == ExprKind::Identifier) {
        const Symbol* symbol = ctx.out.find(callee.text);
        return symbol != nullptr && symbol->kind == SymbolKind::StateSetter;
    }
    if (callee.kind == ExprKind::Member && callee.text == "set" && !callee.operands.empty() &&
        callee.operands[0]->kind == ExprKind::Identifier) {
        const Symbol* symbol = ctx.out.find(callee.operands[0]->text);
        return symbol != nullptr && symbol->kind == SymbolKind::AnimatedValue;
    }
    // `r.focus()` / `r.blur()` on a `useRef` name (issue #226).
    if (callee.kind == ExprKind::Member && (callee.text == "focus" || callee.text == "blur") &&
        !callee.operands.empty() && callee.operands[0]->kind == ExprKind::Identifier) {
        const Symbol* symbol = ctx.out.find(callee.operands[0]->text);
        return symbol != nullptr && symbol->kind == SymbolKind::Ref;
    }
    return false;
}

// Spec 9.2. An empty body is not a handler of this form (least committal; decision 4 is silent).
bool is_setter_only(const Context& ctx, const Expr& arrow) {
    return !arrow.operands.empty() && std::all_of(arrow.operands.begin(), arrow.operands.end(),
                                                  [&](const jsx::ExprPtr& statement) {
                                                      return is_setter_call(ctx, *statement);
                                                  });
}

// Spec 9.3: the body of a native handler is its preview body: setter calls and animated `.set(...)`
// calls, then optionally a final `return <PureExpr>;`. Neither finding stops code generation, so
// both are warnings; lowering gives such a handler no preview.
void check_preview_body(Context& ctx, const jsx::Statement& statement) {
    const Expr& arrow = *statement.init;
    const std::string name = statement.names.empty() ? std::string() : statement.names[0];
    if (arrow.operands.empty()) {
        ctx.sink.report("TESSERA_REACT_NO_PREVIEW_BODY", jsx::Severity::Warning,
                        "native handler `" + name +
                            "` has an empty body, so the designer does nothing for its event",
                        statement.span,
                        "write the preview body: setter calls, and optionally a final `return`");
        return;
    }
    std::size_t count = arrow.operands.size();
    if (arrow.operands.back()->kind == ExprKind::Return) {
        --count;
    }
    const bool in_rules =
        std::all_of(arrow.operands.begin(), arrow.operands.begin() + static_cast<long>(count),
                    [&](const jsx::ExprPtr& body) { return is_setter_call(ctx, *body); });
    if (!in_rules) {
        ctx.sink.report("TESSERA_REACT_PREVIEW_BODY", jsx::Severity::Warning,
                        "the body of native handler `" + name +
                            "` is not a preview body, so the designer does nothing for its event",
                        statement.span,
                        "keep only setter calls and a final `return <expression>;`; the C++ in "
                        "the protected region is the real behavior");
    }
}

void classify_declared_arrow(Context& ctx, const jsx::Statement& statement) {
    const Expr& arrow = *statement.init;
    if (!statement.bind_id.empty()) {
        add_arrow(ctx, arrow, ArrowRole::NativeHandler, statement.bind_id);
        check_preview_body(ctx, statement);
    } else if (is_setter_only(ctx, arrow)) {
        add_arrow(ctx, arrow, ArrowRole::InlineHandler);
    } else {
        const std::string name = statement.names.empty() ? std::string() : statement.names[0];
        ctx.error(
            "TESSERA_REACT_BIND_MISSING",
            "handler `" + name +
                "` has no `// Note: bind::<id>` annotation and its body is not made of setter "
                "calls only",
            statement.span,
            "add `// Note: bind::<id>` on the line before it, or reduce the body to setter "
            "calls");
        add_arrow(ctx, arrow, ArrowRole::Other);
    }
}

// The return of an effect body: only `return () => ...;` as the last statement is a cleanup.
class EffectReturnChecker {
public:
    EffectReturnChecker(Context& ctx, const Expr& effect)
        : ctx_(ctx),
          effect_(effect) {}

    const Expr* find_cleanup() {
        if (effect_.arrow_block_body && !effect_.operands.empty()) {
            const Expr& last = *effect_.operands.back();
            if (last.kind == ExprKind::Return && last.operands.size() == 1 &&
                last.operands[0]->kind == ExprKind::Arrow) {
                valid_ = &last;
                return last.operands[0].get();
            }
        }
        return nullptr;
    }

    void operator()(const Expr& expr) {
        if (expr.kind == ExprKind::Return && &expr != valid_) {
            report(expr);
        }
        for_each_child(expr, *this);
    }

private:
    void report(const Expr& expr) {
        const bool last_statement =
            effect_.arrow_block_body && &expr == effect_.operands.back().get();
        ctx_.error("TESSERA_REACT_EFFECT_RETURN",
                   last_statement ? "the final `return` of an effect must return a cleanup arrow "
                                    "function"
                                  : "`return` is only allowed as the last statement of an effect "
                                    "body, in the form of a cleanup",
                   expr.span, "write the cleanup as `return () => { ... };` at the end");
    }

    Context& ctx_;
    const Expr& effect_;
    const Expr* valid_ = nullptr;
};

void classify_effects(Context& ctx) {
    for (auto& effect : ctx.out.effects) {
        add_arrow(ctx, *effect.body, ArrowRole::Effect, effect.bind_id);
        EffectReturnChecker checker(ctx, *effect.body);
        effect.cleanup = checker.find_cleanup();
        if (effect.cleanup != nullptr) {
            add_arrow(ctx, *effect.cleanup, ArrowRole::Cleanup,
                      effect.bind_id.empty() ? std::string() : effect.bind_id + "_cleanup");
        }
        for (const auto& statement : effect.body->operands) {
            checker(*statement);
        }
    }
}

// Arrows not tied to a statement: a JSX attribute value, a `.map` callback, or anything else.
class ArrowCollector {
public:
    explicit ArrowCollector(Context& ctx)
        : ctx_(ctx) {}

    void operator()(const Expr& expr) {
        visit(expr, false);
    }

    void operator()(const jsx::JsxNode& node) {
        visit_jsx(node);
    }

private:
    void visit(const Expr& expr, bool attribute_value) {
        if (expr.kind == ExprKind::Arrow) {
            add_arrow(ctx_, expr, attribute_value ? ArrowRole::EventArrow : ArrowRole::Other);
        } else if (expr.kind == ExprKind::Call && expr.operands.size() == 2 &&
                   expr.operands[0]->kind == ExprKind::Member && expr.operands[0]->text == "map" &&
                   expr.operands[1]->kind == ExprKind::Arrow) {
            add_arrow(ctx_, *expr.operands[1], ArrowRole::ListCallback);
        }
        for (const auto& operand : expr.operands) {
            visit(*operand, false);
        }
        for (const auto& property : expr.properties) {
            if (property.value) {
                visit(*property.value, false);
            }
        }
        if (expr.jsx) {
            visit_jsx(*expr.jsx);
        }
    }

    void visit_jsx(const jsx::JsxNode& node) {
        for (const auto& attribute : node.attributes) {
            if (attribute.expr) {
                visit(*attribute.expr, attribute.expr->kind == ExprKind::Arrow);
            }
        }
        if (node.expr) {
            visit(*node.expr, false);
        }
        for (const auto& child : node.children) {
            visit_jsx(*child);
        }
    }

    Context& ctx_;
};

} // namespace

void classify_arrows(Context& ctx) {
    const auto& body = ctx.item.body;
    for (const auto& statement : body) {
        if (statement.kind == StatementKind::Const && statement.init &&
            statement.init->kind == ExprKind::Arrow) {
            classify_declared_arrow(ctx, statement);
        }
    }
    classify_effects(ctx);
    ArrowCollector collector(ctx);
    for (const auto& statement : body) {
        if (statement.init) {
            collector(*statement.init);
        }
    }
    if (ctx.item.returned) {
        collector(*ctx.item.returned);
    }
    std::stable_sort(ctx.out.arrows.begin(), ctx.out.arrows.end(),
                     [](const ArrowInfo& a, const ArrowInfo& b) {
                         return a.arrow->span.offset < b.arrow->span.offset;
                     });
}

} // namespace tessera::compiler::rules
