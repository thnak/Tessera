// SPDX-License-Identifier: MIT
//
// The one-call rewrite of the custom-hook expansion pass (spike #659, ADR-065; spec section 4 steps
// 1-7). Split from `hook_expansion.cpp` so neither translation unit passes the 500-line limit.

#include "hook_expansion_expander.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::jsx {

using internal::Substitutions;

namespace {

// The names a hook declares: state, setter, ref, animated value, handler and local. They are the
// `const` statements of its body, which is the same set the rules' symbol table holds.
std::vector<std::string> declared_names(const ModuleItem& hook) {
    std::vector<std::string> names;
    for (const Statement& statement : hook.body) {
        for (const std::string& name : statement.names) {
            names.push_back(name);
        }
    }
    return names;
}

// The `<hook>_<n>__<name>` prefix of spec section 4 step 3.
std::string prefix_for(std::string_view hook, std::size_t ordinal) {
    return std::string(hook) + "_" + std::to_string(ordinal) + "__";
}

// True for an argument that is already a name the expanded component can read: a prop, a state
// variable, an animated value, an earlier hook's output, or a field of one (`props.start`). Spec
// section 4 step 4 substitutes such an argument instead of copying it into a local, which is what
// keeps an effect dependency a plain name (section 4, "Effect dependencies").
bool is_plain_name(const Expr& expr) {
    if (expr.kind == ExprKind::Identifier) {
        return true;
    }
    return expr.kind == ExprKind::Member && expr.text != "[]" && expr.operands.size() == 1 &&
           expr.operands[0]->kind == ExprKind::Identifier;
}

// The span covering every statement of `statements`, or `fallback` when there is none.
Span covering(const std::vector<Statement>& statements, Span fallback) {
    if (statements.empty()) {
        return fallback;
    }
    std::uint32_t begin = statements.front().span.offset;
    std::uint32_t end = statements.front().span.end();
    for (const Statement& statement : statements) {
        begin = std::min(begin, statement.span.offset);
        end = std::max(end, statement.span.end());
    }
    return Span{begin, end - begin};
}

} // namespace

std::size_t Expander::expand_call(std::vector<Statement>& body, std::size_t index,
                                  std::size_t depth, Substitutions& scope) {
    Statement& statement = body[index];
    const std::string_view name = callee_name(*statement.init);
    const HookDeclaration* declaration = resolve(name);
    if (declaration == nullptr || declaration->item == nullptr) {
        return 0;
    }
    const ModuleItem& hook = *declaration->item;

    // Spec section 4 step 1: a cycle is rejected rather than expanded forever.
    if (std::any_of(stack_.begin(), stack_.end(),
                    [&](const Expansion& open) { return open.hook == hook.name; })) {
        report("TESSERA_REACT_HOOK_CYCLE",
               "hook `" + hook.name + "` calls itself, directly or through another hook",
               statement.span, "break the cycle");
        return 0;
    }
    if (depth >= kMaxDepth) {
        report("TESSERA_REACT_HOOK_DEPTH",
               "expanding `" + hook.name + "` is deeper than " + std::to_string(kMaxDepth) +
                   " levels",
               statement.span, "call the hook from the component instead of from another hook");
        return 0;
    }

    // Spec section 4 step 2: the ordinal counts calls of that hook in this component, from 1.
    const std::size_t ordinal = ++ordinals_[hook.name];
    const std::string prefix = prefix_for(hook.name, ordinal);
    const std::string site = hook.name + "_" + std::to_string(ordinal);

    // Spec section 4 step 3: rename every name the hook declares, and every parameter. A parameter
    // whose argument is a plain name is substituted below; one whose argument is an expression is
    // carried by a prologue local of the same prefixed name.
    std::map<std::string, std::string> renames;
    for (const std::string& declared : declared_names(hook)) {
        renames[declared] = prefix + declared;
    }

    // Spec section 4 step 4: bind the parameters. A value parameter whose argument is a plain name
    // is replaced by that name; anything else becomes a `const` local re-evaluated on each update.
    // A trailing parameter with a literal default may be omitted (spec section 2).
    const Expr& call = *statement.init;
    const std::size_t passed = call.operands.size() - 1;
    std::size_t required = hook.hook_params.size();
    while (required > 0 && hook.hook_params[required - 1].default_value) {
        --required;
    }
    if (passed < required || passed > hook.hook_params.size()) {
        report("TESSERA_REACT_HOOK_ARG",
               "`" + hook.name + "` takes " + std::to_string(required) + " to " +
                   std::to_string(hook.hook_params.size()) + " argument(s), but the call passes " +
                   std::to_string(passed),
               call.span, "pass every required parameter");
        return 0;
    }
    std::vector<Statement> prologue;
    Substitutions parameters;
    // Spec section 4, "Effect dependencies": a parameter used in a dependency list must be passed a
    // plain name, because a dependency must be a prop, state variable or animated value and a
    // `const` local is none of those. The check runs before the local is created so the author gets
    // the hint rather than a lowering error about a name the expansion invented.
    std::vector<const Expr*> dependency_arrays;
    for (const Statement& source : hook.body) {
        if (source.init) {
            internal::collect_dependency_arrays(*source.init, dependency_arrays);
        }
    }
    for (std::size_t i = 0; i < hook.hook_params.size(); ++i) {
        const CustomHookParam& param = hook.hook_params[i];
        if (i + 1 > passed) {
            continue;
        }
        const Expr& argument = *call.operands[i + 1];
        if (is_plain_name(argument)) {
            continue;
        }
        bool used_as_dependency = false;
        for (const Expr* array : dependency_arrays) {
            for (const ExprPtr& element : array->operands) {
                if (element && element->kind == ExprKind::Identifier &&
                    element->text == param.name) {
                    used_as_dependency = true;
                }
            }
        }
        if (used_as_dependency) {
            report("TESSERA_REACT_HOOK_DEP",
                   "`" + param.name + "` of `" + hook.name +
                       "` is an effect dependency, so it must be passed a plain name",
                   argument.span,
                   "bind the expression to a `const` in the component and pass that name");
            return 0;
        }
    }
    for (std::size_t i = 0; i < hook.hook_params.size(); ++i) {
        const CustomHookParam& param = hook.hook_params[i];
        if (i + 1 > passed) {
            // Omitted: the declared literal default is the value (spec section 2).
            parameters.values[param.name] = internal::clone_expr(*param.default_value);
            continue;
        }
        const Expr& argument = *call.operands[i + 1];
        if (is_plain_name(argument)) {
            parameters.values[param.name] = internal::clone_expr(argument);
            continue;
        }
        Statement local;
        local.kind = StatementKind::Const;
        local.span = argument.span;
        local.names.push_back(prefix + param.name);
        local.init = internal::clone_expr(argument);
        prologue.push_back(std::move(local));
        // The prologue local carries the value, so the body's reads of the parameter become reads
        // of that local: the rename maps the parameter name to the prefixed one.
        renames[param.name] = prefix + param.name;
    }

    // Spec section 4 step 5: instantiate the body, rename it and splice it at the call position.
    std::vector<Statement> spliced;
    spliced.reserve(hook.body.size());
    for (const Statement& source : hook.body) {
        Statement copy = internal::clone_statement(source);
        if (copy.init) {
            internal::rename(*copy.init, renames);
        }
        if (!copy.bind_id.empty()) {
            copy.bind_id = prefix + copy.bind_id;
            metrics_.bind_ids.push_back(copy.bind_id);
        }
        for (std::string& local : copy.names) {
            const auto found = renames.find(local);
            if (found != renames.end()) {
                local = found->second;
            }
        }
        // Issue #829: a memo/callback dependency list recorded inside the hook names the hook's own
        // declarations and parameters, so it follows the same rename as the body it belongs to.
        for (DependencyName& dep : copy.memo_deps) {
            const auto renamed = renames.find(dep.name);
            if (renamed != renames.end()) {
                dep.name = renamed->second;
            }
        }
        spliced.push_back(std::move(copy));
    }
    // The parameter substitution applies after the rename, so a parameter that is also a declared
    // name cannot shadow the rename.
    for (Statement& spliced_statement : spliced) {
        for (DependencyName& dep : spliced_statement.memo_deps) {
            // A parameter passed a plain name is that name after expansion (spec section 4 step 4),
            // so the recorded entry becomes the name the author passed at the call site.
            const auto value = parameters.values.find(dep.name);
            if (value != parameters.values.end() && value->second &&
                value->second->kind == ExprKind::Identifier) {
                dep.name = value->second->text;
            }
        }
        if (!spliced_statement.init) {
            continue;
        }
        std::vector<Span> whole_value_uses;
        internal::substitute(spliced_statement.init, parameters, whole_value_uses);
    }

    // Spec section 4 step 6: bind the results. The call-site pattern becomes a substitution.
    Substitutions results;
    if (const std::optional<std::string> mismatch =
            internal::bind_results(hook, statement, renames, parameters, results);
        mismatch.has_value()) {
        report("TESSERA_REACT_HOOK_RETURN", *mismatch, statement.span,
               "match the pattern to what `" + hook.name + "` returns");
        return 0;
    }

    // The spliced statements replace the call statement; the prologue goes before them.
    const Span call_span = statement.span;
    const Span spliced_span = covering(spliced, call_span);
    std::vector<Statement> replacement;
    replacement.reserve(prologue.size() + spliced.size());
    for (Statement& local : prologue) {
        replacement.push_back(std::move(local));
    }
    for (Statement& copy : spliced) {
        replacement.push_back(std::move(copy));
    }

    // The spliced statements are expanded before they are written back, so a nested hook is
    // expanded where it appears (spec section 4 step 7). Expanding them in a vector of their own
    // keeps the outer body's indices valid: a nested expansion may change the count.
    stack_.push_back(Expansion{hook.name, call_span, site});
    const Substitutions nested = expand_body(replacement, depth + 1);
    stack_.pop_back();
    if (failed_) {
        return 0;
    }

    // The results were bound from the hook's own source, so a field that reads a nested call's
    // output still names it (`{ bump: c.bump }` where `c` is a hook call). The nested expansion
    // just rewrote those names in the spliced statements, and its scope is what knows the rewrite,
    // so apply it to the results too. Without this the chain stops after one step and the name
    // reaches lowering unresolved -- which is what made the differential fixture "a hook calling a
    // hook" fail on the hook side while its hand-written twin compiled.
    internal::substitute_results(results, nested);

    const std::size_t inserted = replacement.size();
    body.erase(body.begin() + static_cast<long>(index));
    body.insert(body.begin() + static_cast<long>(index),
                std::make_move_iterator(replacement.begin()),
                std::make_move_iterator(replacement.end()));

    // The results apply to the rest of this scope, which the caller's loop does statement by
    // statement; the returned element is the component's own business.
    for (auto& entry : results.values) {
        scope.values[entry.first] = std::move(entry.second);
    }
    for (auto& entry : results.objects) {
        scope.objects[entry.first] = std::move(entry.second);
    }
    metrics_.call_sites.regions.push_back(ExpandedRegion{spliced_span, call_span, site});
    return inserted;
}

} // namespace tessera::compiler::jsx
