// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tessera::compiler::rules {

// One `case` of a recognised reducer (ADR-066 item 4): the action's `type` literal and the record
// its body returns. A `default` case has an empty `action`.
//
// The analyzer resolves the whole form — the helper, its parameters, the action union and the
// `switch` — and records the labels here, so lowering turns a `ReducerInfo` into IR without
// re-reading the module's types. That is the same division every other hook uses: the rules decide
// whether the source is in the accepted form, and lowering emits it.
struct ReducerCase {
    std::string action;              // the `type` literal; empty for `default`
    jsx::Span span;                  // the `case` label, for a diagnostic about that case
    const jsx::Expr* body = nullptr; // the returned value of the case
};

// One `const [state, dispatch] = useReducer(reduce, initial)` statement in its accepted form:
// `reduce` is a module-scope helper whose body is one `switch` over a closed action union, and each
// `case` returns a record of the state type.
struct ReducerInfo {
    std::size_t statement = 0; // index into the component body
    std::string name;          // the state binding (`state`)
    std::string dispatch;      // the dispatch binding (`dispatch`)
    std::string reducer;       // the helper's name as written (`reduce`)
    // The declared state record name (`CounterState`), which is the IR type of the state cell.
    std::string state_type;
    // The `type` literals of the action union, in `case` order (a `default` contributes none).
    std::vector<ReducerCase> cases;
    // True when the helper's `switch` has a `default` case lying outside the union.
    bool has_default = false;
};

} // namespace tessera::compiler::rules
