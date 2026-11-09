// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::rules {

// Recursion depth is bounded by the parser's Limits::max_nesting_depth.

template<typename F> void for_each_jsx_expr(const jsx::JsxNode& node, F& visit) {
    for (const auto& attribute : node.attributes) {
        if (attribute.expr) {
            visit(*attribute.expr);
        }
    }
    if (node.expr) {
        visit(*node.expr);
    }
    for (const auto& child : node.children) {
        for_each_jsx_expr(*child, visit);
    }
}

// Calls `visit` on each direct child expression, including those inside an embedded JSX tree.
template<typename F> void for_each_child(const jsx::Expr& expr, F& visit) {
    for (const auto& operand : expr.operands) {
        visit(*operand);
    }
    for (const auto& property : expr.properties) {
        if (property.value) {
            visit(*property.value);
        }
    }
    if (expr.jsx) {
        for_each_jsx_expr(*expr.jsx, visit);
    }
}

inline bool is_hook_name(std::string_view name) {
    return name.starts_with("use");
}

// The hooks of docs/specs/jsx-subset-v1.md section 7. Library hooks need the hook manifest,
// which does not exist yet (docs/design/jsx-ir-lowering.md section 2).
inline bool is_core_hook(std::string_view name) {
    return name == "useState" || name == "useEffect" || name == "useRef" ||
           name == "useAnimatedValue";
}

// The callee name when `expr` is a call of a plain identifier, else empty.
inline std::string_view callee_name(const jsx::Expr& expr) {
    if (expr.kind == jsx::ExprKind::Call && !expr.operands.empty() &&
        expr.operands[0]->kind == jsx::ExprKind::Identifier) {
        return expr.operands[0]->text;
    }
    return {};
}

struct Read {
    std::string name;
    jsx::Span span;
};

struct ReadOptions {
    // Count `f` of `f(x)` and the receiver of `v.set(x)` as reads. Dependency analysis leaves
    // them out: setters and animated-value writes are not dependencies.
    bool include_callees = false;
    // When non-empty, `props.x` is one read named "props.x" instead of a read of `props`.
    std::string_view props_name;
};

// Identifier reads in source order, honouring arrow parameters that shadow outer names.
std::vector<Read> collect_reads(const jsx::Expr& expr, const ReadOptions& options);

} // namespace tessera::compiler::rules
