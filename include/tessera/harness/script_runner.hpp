// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/command_handler.hpp"
#include "tessera/harness/script.hpp"
#include "tessera/harness/session.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tessera::harness {

// Caps for one run. `max_steps` bounds handler calls; the time budgets use the injected
// SessionClock, never a wall clock inside the runner (section 7: structural/golden runs use
// simulation time only). `run_timeout_us == 0` means the step cap is the only total bound.
struct ScriptBudget {
    std::size_t max_steps = kMaxScriptSteps;
    std::uint64_t step_timeout_us = 5'000'000; // per-step ceiling, mirroring SessionLimits
    std::uint64_t run_timeout_us = 0;          // whole-run deadline, 0 = unbounded beyond max_steps
};

struct ScriptStepReport {
    std::size_t index = 0;
    ScriptAction action = ScriptAction::Click;
    std::optional<UiId> ui_id;
    std::optional<std::string> item_key;
    bool ok = false;
    std::uint64_t elapsed_us = 0;
    JsonValue result;          // canonical command result on success
    std::string artifact_path; // trace/audit path the handler wrote, or a snapshot this step wrote
    ScriptError error;         // meaningful when ok is false
};

// Section 5 `report.json` model, built incrementally so a caller can inspect a cancelled or
// capped run. `error` is set only for run-level failures (step cap, run budget, cancellation);
// a failing step carries its own error in `steps`.
struct ScriptReport {
    bool passed = false;
    bool cancelled = false;
    std::uint64_t elapsed_us = 0;
    std::size_t steps_executed = 0;
    std::size_t steps_total = 0;
    std::vector<ScriptStepReport> steps;
    std::optional<ScriptError> error;
};

// Canonical report object (section 5 fields: index, action, target id, result, duration, error,
// artifact paths), in the fixed member order documented in the spec.
[[nodiscard]] JsonValue encode_script_report(const ScriptReport& report);

// Runs one parsed script against the CommandHandler seam, one bounded step per pump().
//
// Threading: like SessionExecutor, the runner owns no thread and never waits, sleeps, or polls.
// pump() makes at most one synchronous handler call, so a host can drive it from a RuntimeLoop
// tick; a handler that cannot finish reports Timeout instead of parking the UI thread. Commands
// stay serialized, so the runner is deterministic for a fixed script, handler, sink and clock.
class ScriptRunner {
public:
    ScriptRunner(Script script, CommandHandler& handler, ArtifactSink& sink, SessionClock& clock,
                 ScriptBudget budget = {});
    ~ScriptRunner() = default;

    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    // Executes at most one step. Returns true when another pump may make progress (including the
    // pump that finishes the run), false once the run is finished.
    [[nodiscard]] bool pump();

    // Requests cancellation. A synchronous handler cannot be preempted; the next pump records
    // Cancelled and stops, and no further step is dispatched.
    void cancel() noexcept;

    [[nodiscard]] bool finished() const noexcept {
        return finished_;
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_;
    }
    [[nodiscard]] std::size_t next_step() const noexcept {
        return next_;
    }
    [[nodiscard]] const ScriptReport& report() const noexcept {
        return report_;
    }

private:
    // False means the run stopped on this step; the step error is already recorded.
    [[nodiscard]] bool execute(const ScriptStep& step, std::uint64_t start_us,
                               std::uint64_t step_budget_us);
    void record_step(ScriptStepReport step_report);
    void finalize() noexcept;
    void fail_run(ScriptErrorCode code, std::string message, std::string detail = {});
    [[nodiscard]] std::uint64_t elapsed_since(std::uint64_t start_us) const noexcept;

    Script script_;
    CommandHandler& handler_;
    ArtifactSink& sink_;
    SessionClock& clock_;
    ScriptBudget budget_;
    ScriptReport report_;
    std::uint64_t start_us_ = 0;
    std::uint64_t run_deadline_us_ = 0;
    std::size_t next_ = 0;
    bool finished_ = false;
    bool cancelled_ = false;
};

} // namespace tessera::harness
