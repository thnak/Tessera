// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/value.hpp"
#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/expr.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tessera::compiler::eval {

// An expression or action the evaluator could not run. The front end rejects unsupported
// constructs, so an error here means the IR does not match its declared types (a bug in the
// caller or in lowering), never a user mistake. `code` is stable and grep-able.
struct EvalError {
    std::string code;
    std::string message;
};

// A runtime diagnostic the language defines for a total operation (ir-v1 section 4.1), reported
// without failing the evaluation.
struct Diagnostic {
    std::string code;
    std::string message;
};

// Every readable name: state (`state::count`), props (`prop::x`), animated values (`anim::x`),
// environment, handler parameters and loop locals by the id the IR refers to them with.
using Scope = std::unordered_map<std::string, Value>;

// Evaluates IR expressions and runs action lists with the semantics of docs/specs/ir-v1.md
// sections 4.1 and 4.2, which generated C++ must match (docs/design/designer.md section 2, the
// differential suite): int is signed 32-bit and wraps, float is binary64, `%`, `/` and the
// conversions follow the numeric table, and the pure functions are exactly the allowlist.
//
// One Evaluator is one session: `TESSERA_IR_INT_MODULO_ZERO` is reported once per expression node
// for the session's lifetime. Evaluation is pure and deterministic (no locale, clock or
// randomness); nesting deeper than kMaxDepth is an error rather than a stack overflow.
class Evaluator {
public:
    static constexpr std::size_t kMaxDepth = 256;

    [[nodiscard]] std::expected<Value, EvalError> evaluate(const ir::Expr& expr,
                                                           const Scope& scope);

    // Runs `actions` in order against `scope`, as the generated handler does: each `set` or
    // `animSet` assigns its target immediately, so a later action reads the earlier one's result.
    // The target must already exist (a declared state or animated value) and keeps its type; an
    // `int` may be assigned to a `float`, as C++ converts it. On an error the earlier actions stay
    // applied.
    [[nodiscard]] std::expected<void, EvalError> run(std::span<const ir::Action> actions,
                                                     Scope& scope);

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    [[nodiscard]] std::expected<Value, EvalError> eval(const ir::Expr& expr, const Scope& scope,
                                                       std::size_t depth);
    [[nodiscard]] std::expected<Value, EvalError>
    eval_binary(const ir::Expr& expr, const Scope& scope, std::size_t depth);
    [[nodiscard]] std::expected<Value, EvalError>
    eval_template(const ir::Expr& expr, const Scope& scope, std::size_t depth);
    [[nodiscard]] std::expected<Value, EvalError> eval_call(const ir::Expr& expr,
                                                            const Scope& scope, std::size_t depth);
    void report_modulo_zero(const ir::Expr& expr);

    std::vector<Diagnostic> diagnostics_;
    std::unordered_set<const ir::Expr*> reported_;
};

} // namespace tessera::compiler::eval
