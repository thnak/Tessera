// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/hook_manifest.hpp"

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

// A hook name is `use` plus an uppercase letter (docs/specs/jsx-subset-v1.md section 7,
// docs/specs/custom-hooks-v1.md section 2). This is only the *shape* test; whether a call is a hook
// is decided against the core set and the manifest (`is_known_hook`), because the custom-hook
// expansion names every name it introduces `<hook>_<n>__<name>` (spec section 4 step 3) and such a
// name — `usePagination_1__setPage` — has the shape of a hook name without being one.
inline bool is_hook_name(std::string_view name) {
    return name.size() > 3 && name.starts_with("use") && name[3] >= 'A' && name[3] <= 'Z';
}

// The hooks of docs/specs/jsx-subset-v1.md section 7. Library hooks need the hook manifest, which
// does not exist yet (docs/design/jsx-ir-lowering.md section 2).
//
// `useId` and `useLayoutEffect` are core hooks since ADR-066 item 1: `useId` yields a stable id
// from the instance key and the call ordinal, and `useLayoutEffect` is `useEffect` with the same
// timing (there is no browser paint to order against). `useMemo` and `useCallback` are handled
// separately, because only their safe forms are accepted (react-compat.md section 4.2).
//
// `useReducer` is a core hook since ADR-066 item 4. Its *form* is checked by its own pass
// (reducer.cpp) because it is defined by a module-scope helper and the type aliases beside it, but
// it is listed here so the placement and nesting rules treat it as a hook rather than as an
// unknown name.
inline bool is_core_hook(std::string_view name) {
    return name == "useState" || name == "useEffect" || name == "useRef" ||
           name == "useAnimatedValue" || name == "useId" || name == "useLayoutEffect" ||
           name == "useReducer" ||
           // ADR-066 item 5 (issue #802): `useContext` is a core hook whose form its own pass
           // (context.cpp) checks against the module's `createContext` consts.
           name == "useContext" ||
           // ADR-050 section 3.1 (issue #177): `useService<T>()`, whose form its own pass
           // (service.cpp) checks against the project's services manifest.
           name == "useService" ||
           // ADR-050 open question 6: `useObservable(svc.member)`, checked by observable.cpp.
           name == "useObservable";
}

// True for a name that is a hook call: a core hook, or a hook the manifest declares. The expansion
// of a custom hook introduces names of the shape `useX_1__y` (docs/specs/custom-hooks-v1.md
// section 4 step 3), which `is_hook_name` accepts but which are ordinary names, so a check that
// would report an unknown hook must ask this rather than the shape test.
inline bool is_known_hook(std::string_view name, const jsx::HookManifest& manifest) {
    return is_core_hook(name) || manifest.find(name) != nullptr;
}

// True for a name the custom-hook expansion introduced: `<hook>_<n>__<name>` (spec section 4 step
// 3). Such a name is never a hook call, whatever its shape.
inline bool is_expansion_name(std::string_view name) {
    const std::size_t marker = name.find("__");
    if (marker == std::string_view::npos || marker < 3) {
        return false;
    }
    const std::string_view head = name.substr(0, marker);
    const std::size_t underscore = head.rfind('_');
    if (underscore == std::string_view::npos || underscore + 1 >= head.size()) {
        return false;
    }
    for (std::size_t i = underscore + 1; i < head.size(); ++i) {
        if (head[i] < '0' || head[i] > '9') {
            return false;
        }
    }
    return is_hook_name(head.substr(0, underscore));
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
    // Also report `name.member` for a member read of a plain identifier (`feed.status`), in
    // addition to the read of `name`. The effect rule needs it for resource reads
    // (pending-ui-v1.md section 3.1); a name that is not a resource is ignored by its resolver.
    bool member_reads = false;
};

// Identifier reads in source order, honouring arrow parameters that shadow outer names.
std::vector<Read> collect_reads(const jsx::Expr& expr, const ReadOptions& options);

} // namespace tessera::compiler::rules
