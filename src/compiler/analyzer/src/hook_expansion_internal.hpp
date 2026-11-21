// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

// Internals of the custom-hook expansion pass (spike #659, ADR-065). Not installed; only
// src/compiler/analyzer includes it.
namespace tessera::compiler::jsx::internal {

// Deep copies. The expansion instantiates one copy of a hook body per call site, so the module's
// declaration is never modified and two call sites cannot share a node.
[[nodiscard]] ExprPtr clone_expr(const Expr& expr);
[[nodiscard]] JsxPtr clone_jsx(const JsxNode& node);
[[nodiscard]] TypePtr clone_type(const TypeNode& type);
[[nodiscard]] Statement clone_statement(const Statement& statement);

// What a call-site name stands for after expansion (spec section 4 step 6). The values are owned
// templates: each use gets a fresh copy, so two reads of `p.page` are two independent nodes.
struct Substitutions {
    // A name bound to one value: an array element, an object field, or a single-value return.
    std::map<std::string, ExprPtr> values;
    // A whole-value binding of an object return: `p` -> its fields, so `p.page` resolves and `p`
    // alone is `TESSERA_REACT_HOOK_RETURN` (spec section 3).
    std::map<std::string, std::map<std::string, ExprPtr>> objects;

    [[nodiscard]] bool empty() const {
        return values.empty() && objects.empty();
    }
};

// Replaces every identifier `subs` names, recursing into children. `slot` itself is replaced when
// it is a substituted name or a `p.field` of a whole-value binding. A whole-value name used as a
// value is appended to `whole_value_uses` instead of being replaced.
void substitute(ExprPtr& slot, const Substitutions& subs, std::vector<Span>& whole_value_uses);
void substitute(JsxNode& node, const Substitutions& subs, std::vector<Span>& whole_value_uses);

// Applies `subs` to every expression a scope holds, in place. A hook's results are bound from its
// own source, so a field that reads a nested call's output still names it (`{ bump: c.bump }` where
// `c` is a hook call); the nested expansion is what knows the rewrite, so the caller applies its
// scope to the results as well as to the spliced statements. Without this the chain stops after one
// step and the name reaches lowering unresolved.
void substitute_results(Substitutions& results, const Substitutions& subs);

// Renames every identifier `names` maps, honouring arrow parameters that shadow an outer name.
void rename(Expr& expr, const std::map<std::string, std::string>& names);
void rename(JsxNode& node, const std::map<std::string, std::string>& names);

// The `useEffect` dependency arrays of `expr`, so the expansion can tell whether a parameter is
// used as a dependency (spec section 4, "Effect dependencies").
void collect_dependency_arrays(const Expr& expr, std::vector<const Expr*>& out);

// Binds the results of a call (spec section 4 step 6): reads `hook.hook_return` through the
// call-site pattern of `statement` and fills `results`. Returns the message of the diagnostic to
// report when the pattern does not match the return, or nullopt on success. `renames` and
// `parameters` are applied to the returned expression first, so it names the expansion's own names.
[[nodiscard]] std::optional<std::string>
bind_results(const ModuleItem& hook, const Statement& statement,
             const std::map<std::string, std::string>& renames, const Substitutions& parameters,
             Substitutions& results);

} // namespace tessera::compiler::jsx::internal
