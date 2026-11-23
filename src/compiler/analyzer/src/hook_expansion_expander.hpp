// SPDX-License-Identifier: MIT
//
// Internals of the custom-hook expansion pass (spike #659, ADR-065; docs/specs/custom-hooks-v1.md
// section 4). Not installed; only src/compiler/analyzer includes it.
//
// `Expander` is split across two translation units so neither passes the 500-line limit:
// `hook_expansion.cpp` holds the component/body walk and the diagnostic plumbing, and
// `hook_expansion_call.cpp` holds the one-call rewrite (`expand_call`), which is where most of the
// spec's numbered steps live.

#pragma once

#include "hook_expansion_internal.hpp"
#include "tessera/compiler/jsx/hook_expansion.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::jsx {

// Spec section 7: expansion depth 8, and 400 statements added to one component (a warning at 200).
inline constexpr std::size_t kMaxDepth = 8;
inline constexpr std::size_t kMaxStatements = 400;
inline constexpr std::size_t kWarnStatements = 200;

// The callee name when `expr` is a call of a plain identifier, else empty. Used by both the body
// walk and the call rewrite, so it lives here rather than in either translation unit.
inline std::string_view callee_name(const Expr& expr) {
    if (expr.kind == ExprKind::Call && !expr.operands.empty() &&
        expr.operands[0]->kind == ExprKind::Identifier) {
        return expr.operands[0]->text;
    }
    return {};
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
    internal::Substitutions expand_component(ModuleItem& component);

    [[nodiscard]] bool failed() const {
        return failed_;
    }

private:
    const HookDeclaration* resolve(std::string_view name) const;
    // Expands the calls of `body` in place and returns the substitutions of that scope.
    internal::Substitutions expand_body(std::vector<Statement>& body, std::size_t depth);
    // Replaces `body[index]` by the expansion of its call. Returns the statements inserted.
    std::size_t expand_call(std::vector<Statement>& body, std::size_t index, std::size_t depth,
                            internal::Substitutions& scope);
    void report(std::string_view code, std::string message, Span span, std::string suggestion = {});
    [[nodiscard]] std::optional<RelatedSpan> related_for() const;

    // One hook currently being expanded: its name for cycle detection, and the call that started
    // it, so a diagnostic raised inside a spliced body can name the call the author wrote.
    struct Expansion {
        std::string hook;
        Span call_span;
        std::string site; // `<hook>_<n>`
    };

    const std::vector<HookDeclaration>& declarations_;
    DiagnosticSink& sink_;
    ExpansionMetrics& metrics_;
    const SourceMap& map_;
    // Calls of one hook in the component being expanded, for the ordinal of spec section 4 step 2.
    std::map<std::string, std::size_t> ordinals_;
    std::vector<Expansion> stack_; // outermost first
    bool failed_ = false;
};

} // namespace tessera::compiler::jsx
