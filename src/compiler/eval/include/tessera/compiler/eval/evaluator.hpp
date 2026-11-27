// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/pending.hpp"
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

    // ADR-067 spike #661, off by default: the pending-UI session a transition-marked action runs
    // against. Null (the default) means a transition mark is inert, which is what keeps every
    // existing evaluation byte-identical. The session is the evaluator's half of the differential
    // test: it runs the same `tessera::ui::pending` state machine generated C++ runs.
    // A `callProp` action calls a callback prop of the component, which belongs to the caller. The
    // evaluator does not model the caller, so a host that does (the preview session, for a
    // component instance) installs a sink: it receives the `prop::<name>` target and whether the
    // call carries a payload, after the action's guard held. Without a sink the call is skipped.
    using PropCallSink = void (*)(void* context, const std::string& target, bool has_payload);
    void set_prop_call_sink(PropCallSink sink, void* context) noexcept {
        prop_sink_ = sink;
        prop_sink_context_ = context;
    }

    void notify_prop_call(const ir::Action& action) const {
        if (prop_sink_ != nullptr) {
            prop_sink_(prop_sink_context_, action.target, action.value != nullptr);
        }
    }

    // A `callService` action calls a method of a service the component requires (ADR-050
    // section 3.1 item 4). The evaluator has no services, so a host that models them (the preview,
    // the Studio mock services) installs a sink: it receives the `svc::<name>` target, the method
    // and the evaluated arguments, after the action's guard held. Without a sink the call is
    // skipped.
    using ServiceCallSink = void (*)(void* context, const std::string& target,
                                     const std::string& method, const std::vector<Value>& args);
    void set_service_call_sink(ServiceCallSink sink, void* context) noexcept {
        service_sink_ = sink;
        service_sink_context_ = context;
    }

    void set_pending_session(PendingSession* session) noexcept {
        pending_ = session;
    }
    [[nodiscard]] PendingSession* pending_session() const noexcept {
        return pending_;
    }

private:
    // Actions that leave the value model: focus, `callProp` and `callService`. They are skipped by
    // the state/anim assignment path and handled by `run_host_action` (evaluator_service.cpp).
    [[nodiscard]] static bool is_host_action(ir::ActionKind kind) noexcept {
        return kind == ir::ActionKind::Focus || kind == ir::ActionKind::Blur ||
               kind == ir::ActionKind::CallProp || kind == ir::ActionKind::CallService;
    }
    [[nodiscard]] std::expected<void, EvalError> run_host_action(const ir::Action& action,
                                                                 const Scope& scope);
    // A `ref` the scope does not hold: a resource `.status` the bound pending session knows
    // (#656), otherwise `TESSERA_IR_EVAL_UNKNOWN_REF` (evaluator_service.cpp).
    [[nodiscard]] std::expected<Value, EvalError> unresolved_ref(const ir::Expr& expr) const;
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
    PendingSession* pending_ = nullptr;
    PropCallSink prop_sink_ = nullptr;
    void* prop_sink_context_ = nullptr;
    ServiceCallSink service_sink_ = nullptr;
    void* service_sink_context_ = nullptr;
};

} // namespace tessera::compiler::eval
