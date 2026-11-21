// SPDX-License-Identifier: MIT

// The custom-hook expansion pass (spike #659, ADR-065; docs/specs/custom-hooks-v1.md section 4).
//
// For each call site, in source order: resolve the callee, instantiate a copy of the hook's body,
// rename every name the hook declares to `<hook>_<n>__<name>`, bind the parameters, splice the
// statements at the call position and substitute the returned values. The result is exactly what
// the author could have written by hand, so the rules, lowering and code generation see an
// ordinary component and need no change.
//
// The pass is off by default (`FrontEndOptions::custom_hooks`): with it off this file does nothing
// and the front end keeps the `TESSERA_REACT_CUSTOM_HOOK` rejection.

#include "hook_expansion_internal.hpp"
#include "tessera/compiler/jsx/hook_expansion.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::jsx {
namespace {

using internal::Substitutions;

// Spec section 7: expansion depth 8, and 400 statements added to one component (a warning at 200).
constexpr std::size_t kMaxDepth = 8;
constexpr std::size_t kMaxStatements = 400;
constexpr std::size_t kWarnStatements = 200;

// The callee name when `expr` is a call of a plain identifier, else empty.
std::string_view callee_name(const Expr& expr) {
    if (expr.kind == ExprKind::Call && !expr.operands.empty() &&
        expr.operands[0]->kind == ExprKind::Identifier) {
        return expr.operands[0]->text;
    }
    return {};
}

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

class Expander {
public:
    Expander(const std::vector<HookDeclaration>& declarations, const SourceMap& map,
             DiagnosticSink& sink, ExpansionMetrics& metrics)
        : declarations_(declarations),
          sink_(sink),
          metrics_(metrics),
          map_(map) {}

    // Expands the body of one component in place and returns the substitutions its call sites
    // introduced, so the caller can apply them to the returned element too.
    Substitutions expand_component(ModuleItem& component);

    [[nodiscard]] bool failed() const {
        return failed_;
    }

private:
    const HookDeclaration* resolve(std::string_view name) const;
    // Expands the calls of `body` in place and returns the substitutions of that scope.
    Substitutions expand_body(std::vector<Statement>& body, std::size_t depth);
    // Replaces `body[index]` by the expansion of its call. Returns the statements inserted.
    std::size_t expand_call(std::vector<Statement>& body, std::size_t index, std::size_t depth,
                            Substitutions& scope);
    void report(std::string_view code, std::string message, Span span, std::string suggestion = {},
                std::optional<RelatedSpan> related = std::nullopt);
    [[nodiscard]] std::optional<RelatedSpan> related_for(Span span) const;

    const std::vector<HookDeclaration>& declarations_;
    DiagnosticSink& sink_;
    ExpansionMetrics& metrics_;
    const SourceMap& map_;
    // Calls of one hook in the component being expanded, for the ordinal of spec section 4 step 2.
    std::map<std::string, std::size_t> ordinals_;
    std::vector<std::string> stack_; // the hooks currently being expanded, for cycle detection
    bool failed_ = false;
};

const HookDeclaration* Expander::resolve(std::string_view name) const {
    for (const HookDeclaration& declaration : declarations_) {
        if (declaration.name == name) {
            return &declaration;
        }
    }
    return nullptr;
}

void Expander::report(std::string_view code, std::string message, Span span, std::string suggestion,
                      std::optional<RelatedSpan> related) {
    failed_ = true;
    sink_.report(code, Severity::Error, std::move(message), span, std::move(suggestion),
                 std::move(related));
}

// Spec section 8: a diagnostic whose primary span is inside a hook carries the call site as its
// related span, with the text "expanded here, from <hook>_<n>". The expansion's own diagnostics
// are reported at the call site already, so they need no related span; this is for the rules.
std::optional<RelatedSpan> Expander::related_for(Span span) const {
    const ExpandedRegion* region = metrics_.call_sites.find(span);
    if (region == nullptr) {
        return std::nullopt;
    }
    return RelatedSpan{map_.resolve(region->call_span), "expanded here, from " + region->hook};
}

Substitutions Expander::expand_component(ModuleItem& component) {
    Substitutions scope = expand_body(component.body, 0);
    if (failed_ || scope.empty() || !component.returned) {
        return scope;
    }
    std::vector<Span> whole_value_uses;
    internal::substitute(*component.returned, scope, whole_value_uses);
    for (const Span& span : whole_value_uses) {
        report("TESSERA_REACT_HOOK_RETURN",
               "a whole-value binding of an object return has no run-time record", span,
               "read a field (`p.page`) or bind the fields with an object pattern",
               related_for(span));
    }
    return scope;
}

Substitutions Expander::expand_body(std::vector<Statement>& body, std::size_t depth) {
    Substitutions scope;
    // A statement may splice in several, so the loop re-reads the size: the spliced statements are
    // themselves expanded (spec section 4 step 7).
    for (std::size_t index = 0; index < body.size(); ++index) {
        if (!body[index].init) {
            continue;
        }
        // The scope so far applies to this statement before it is expanded, so an argument that
        // reads an earlier call site's output is already the hook's own source.
        std::vector<Span> whole_value_uses;
        internal::substitute(body[index].init, scope, whole_value_uses);
        for (const Span& span : whole_value_uses) {
            report("TESSERA_REACT_HOOK_RETURN",
                   "a whole-value binding of an object return has no run-time record", span,
                   "read a field (`p.page`) or bind the fields with an object pattern",
                   related_for(span));
        }
        const std::string_view name = callee_name(*body[index].init);
        if (name.empty() || resolve(name) == nullptr) {
            continue;
        }
        const std::size_t inserted = expand_call(body, index, depth, scope);
        if (failed_) {
            return scope;
        }
        index += inserted == 0 ? 0 : inserted - 1;
    }
    return scope;
}

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
    if (std::find(stack_.begin(), stack_.end(), hook.name) != stack_.end()) {
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
        spliced.push_back(std::move(copy));
    }
    // The parameter substitution applies after the rename, so a parameter that is also a declared
    // name cannot shadow the rename.
    for (Statement& spliced_statement : spliced) {
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
    stack_.push_back(hook.name);
    expand_body(replacement, depth + 1);
    stack_.pop_back();
    if (failed_) {
        return 0;
    }

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

} // namespace

std::vector<HookDeclaration> collect_hook_declarations(const Module& module, DiagnosticSink& sink) {
    std::vector<HookDeclaration> declarations;
    for (const ModuleItem& item : module.items) {
        if (item.kind != ItemKind::Hook) {
            continue;
        }
        const bool duplicate =
            std::any_of(declarations.begin(), declarations.end(),
                        [&](const HookDeclaration& known) { return known.name == item.name; });
        if (duplicate) {
            sink.error("TESSERA_REACT_HOOK_FORM",
                       "hook `" + excerpt(item.name) + "` is declared twice in this file",
                       item.name_span, "rename one of them");
            continue;
        }
        declarations.push_back(HookDeclaration{item.name, &item});
    }
    return declarations;
}

const ExpandedRegion* CallSiteMap::find(Span span) const {
    const ExpandedRegion* best = nullptr;
    for (const ExpandedRegion& region : regions) {
        if (span.offset < region.span.offset || span.end() > region.span.end()) {
            continue;
        }
        if (best == nullptr || region.span.length < best->span.length) {
            best = &region;
        }
    }
    return best;
}

ExpansionMetrics expand_custom_hooks(Module& module, const SourceMap& map,
                                     const FrontEndOptions& options, DiagnosticSink& sink) {
    (void)map;
    ExpansionMetrics metrics;
    if (!options.custom_hooks) {
        return metrics;
    }
    const std::vector<HookDeclaration> declarations = collect_hook_declarations(module, sink);
    if (declarations.empty()) {
        return metrics;
    }
    Expander expander(declarations, map, sink, metrics);
    for (ModuleItem& item : module.items) {
        if (item.kind != ItemKind::Component) {
            continue;
        }
        const std::size_t before = item.body.size();
        expander.expand_component(item);
        const std::size_t added = item.body.size() - before;
        metrics.added_statements.push_back(added);
        if (added > kMaxStatements) {
            sink.error("TESSERA_REACT_HOOK_SIZE",
                       "expanding the hooks of `" + excerpt(item.name) + "` adds " +
                           std::to_string(added) + " statements, more than " +
                           std::to_string(kMaxStatements),
                       item.name_span, "split the component or inline the hook");
        } else if (added > kWarnStatements) {
            sink.report("TESSERA_REACT_HOOK_SIZE", Severity::Warning,
                        "expanding the hooks of `" + excerpt(item.name) + "` adds " +
                            std::to_string(added) + " statements",
                        item.name_span, "split the component or inline the hook");
        }
        if (expander.failed()) {
            return metrics;
        }
    }
    return metrics;
}

} // namespace tessera::compiler::jsx
