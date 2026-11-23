// SPDX-License-Identifier: MIT

// `useMemo`/`useCallback` dependency lists are checked for agreement, not exactness (issue #829;
// docs/specs/jsx-intrinsics-v1/react-compat.md section 4.2). The inferred reactive set is
// authoritative: Tessera recomputes on it, where React would keep a stale value from a wrong list.
// So a disagreement warns and names what to fix; it is never an error. An element that is not a
// plain name is still an error, the same rule `useEffect` has.
//
// The parser desugared the safe forms and recorded the list on the statement (`Statement::memo_*`);
// this pass reads that record. `useEffect` keeps its exact rule in `effect_deps.cpp`.

#include "dependency_names.hpp"
#include "reporter.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tessera::compiler::rules {
namespace {

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// One desugared memo/callback statement: every prop, state variable or animated value its inlined
// body reads must agree with the list the author wrote. The body is the statement's initializer,
// already the wrapped expression (memo) or arrow (callback), so `required_names` inlines any local
// it reads -- a memo over another memo resolves through to props and state.
void check_memo(Context& ctx, const jsx::Statement& statement) {
    if (statement.memo_hook.empty() || !statement.init) {
        return;
    }
    // A non-name element was recorded by the parser; it is an error here because only the rules
    // pass has the message the effect rule uses. The first such element is the span.
    if (statement.memo_deps_error_span.length != 0) {
        ctx.error("TESSERA_REACT_MEMO_DEPS",
                  "a memo dependency must be a plain name, not an expression",
                  statement.memo_deps_error_span,
                  "list the prop, state variable or animated value by name");
        // Still compare the readable names so a mixed list reports each problem, as `useEffect`
        // does after its non-name error.
    }
    const std::vector<std::string> required = required_names(ctx, *statement.init);
    std::vector<std::string> declared;
    for (const jsx::DependencyName& dep : statement.memo_deps) {
        if (contains(declared, dep.name)) {
            ctx.warning("TESSERA_REACT_MEMO_DEPS", "`" + dep.name + "` is listed twice", dep.span,
                        "remove the duplicate");
            continue;
        }
        declared.push_back(dep.name);
    }
    // A listed name the body does not read is dead text: it never changes the result, so the
    // suggestion is to remove it. The span is the entry, so the warning points at the word.
    for (const jsx::DependencyName& dep : statement.memo_deps) {
        if (!contains(required, dep.name)) {
            ctx.warning("TESSERA_REACT_MEMO_DEPS",
                        "`" + dep.name + "` is listed but the body does not read it", dep.span,
                        "remove `" + dep.name + "` from the dependency array");
        }
    }
    // A read the body does not list is the stale-value case: React would keep the old value. The
    // span is the whole array, because there is no missing token to point at. An absent array is
    // accepted (issue #829 decision 3): there is no declaration to disagree with, and recomputing
    // on the inferred set is what React does without a list too.
    if (!statement.memo_array) {
        return;
    }
    for (const std::string& name : required) {
        if (contains(declared, name)) {
            continue;
        }
        const bool props_member =
            !ctx.item.props_name.empty() && name.starts_with(ctx.item.props_name + ".");
        ctx.warning("TESSERA_REACT_MEMO_DEPS",
                    "`" + name +
                        "` is read but not listed; Tessera recomputes when it changes, React would "
                        "keep the old value",
                    statement.memo_deps_span,
                    props_member ? "destructure the props parameter so the prop has a name to list"
                                 : "add `" + name + "` to the dependency array");
    }
}

} // namespace

void check_memo_deps(Context& ctx) {
    for (std::size_t index = 0; index < ctx.item.body.size(); ++index) {
        check_memo(ctx, ctx.item.body[index]);
    }
}

} // namespace tessera::compiler::rules
