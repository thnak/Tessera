// SPDX-License-Identifier: MIT
//
// `BackgroundJob`: the contract for OS-scheduled background work (issue #1043, split from #453;
// design: docs/design/background-work.md section 6 and docs/design/background-jobs.md).
//
// Three pieces, all OS-free so they can be tested on every lane with a fake:
//
//   * `BackgroundJob` - what the app declares: a stable id, the constraints it wants the OS to
//     honour before waking it, and a `start` that spawns its work into the job's `TaskScope`;
//   * `JobContext` - what the job gets: that scope, the clock, the hard time budget, a checkpoint
//     callback slot and a one-shot `finish`;
//   * `JobScheduler` - the *only* seam to an OS scheduler (WorkManager, `BGTaskScheduler`). A
//     real back end maps `JobConstraints` onto the platform and, when the OS wakes the process,
//     starts a `HeadlessJobHost` for the id. Nothing in this header names a platform type.

#pragma once

#include "tessera/appfw/task_scope.hpp"
#include "tessera/runtime/clock.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tessera::appfw {

// What the OS must have true before it wakes the app for the job. `earliest_after_seconds` is a
// delay from scheduling, not a wall-clock instant, so the contract needs no calendar.
struct JobConstraints {
    bool requires_network = false;
    bool requires_charging = false;
    double earliest_after_seconds = 0.0;
};

// What the job reports through `JobContext::finish`.
enum class JobResult : std::uint8_t {
    Success,
    // Ask the OS to run it again later (WorkManager `Result.retry`, iOS reschedule).
    Retry,
    Failed,
};

// What the host concluded about one run. `BudgetExpired` and `Stopped` both ran the checkpoint
// first and then closed the scope; they differ in who ended the run (our budget margin or the OS
// expiration handler).
enum class JobOutcome : std::uint8_t {
    Pending,
    Success,
    RetryRequested,
    Failed,
    BudgetExpired,
    Stopped,
    // Never reached the job: unknown id, a required service failed to start, or `start` refused.
    NotStarted,
};

[[nodiscard]] constexpr const char* to_string(JobOutcome outcome) noexcept {
    switch (outcome) {
    case JobOutcome::Pending:
        return "pending";
    case JobOutcome::Success:
        return "success";
    case JobOutcome::RetryRequested:
        return "retry-requested";
    case JobOutcome::Failed:
        return "failed";
    case JobOutcome::BudgetExpired:
        return "budget-expired";
    case JobOutcome::Stopped:
        return "stopped";
    case JobOutcome::NotStarted:
        return "not-started";
    }
    return "pending";
}

class JobContext {
public:
    JobContext(TaskScope& scope, runtime::Clock& clock, double deadline_seconds,
               double budget_seconds)
        : scope_(&scope),
          clock_(&clock),
          deadline_seconds_(deadline_seconds),
          budget_seconds_(budget_seconds) {}

    JobContext(const JobContext&) = delete;
    JobContext& operator=(const JobContext&) = delete;

    // Children of the job are spawned here; a budget expiry cancels and closes this scope.
    [[nodiscard]] TaskScope& scope() noexcept {
        return *scope_;
    }
    [[nodiscard]] runtime::Clock& clock() noexcept {
        return *clock_;
    }
    // The budget the host enforces: the OS budget minus the host's safety margin.
    [[nodiscard]] double budget_seconds() const noexcept {
        return budget_seconds_;
    }
    [[nodiscard]] double remaining_seconds() const {
        const double left = deadline_seconds_ - clock_->now_seconds();
        return left > 0.0 ? left : 0.0;
    }

    // Runs once on the UI thread just before the host cancels the job at budget or at an OS stop
    // (the iOS expiration handler's job): save partial progress here. Must not block.
    void set_checkpoint(std::function<void()> checkpoint) {
        checkpoint_ = std::move(checkpoint);
    }
    // First call wins; later calls (a racing child) are ignored.
    void finish(JobResult result) noexcept {
        if (!finished_) {
            finished_ = true;
            result_ = result;
        }
    }
    [[nodiscard]] bool finished() const noexcept {
        return finished_;
    }
    [[nodiscard]] JobResult result() const noexcept {
        return result_;
    }

    // Host-side: returns whether a checkpoint was installed and ran.
    bool run_checkpoint() {
        if (!checkpoint_) {
            return false;
        }
        auto checkpoint = std::move(checkpoint_);
        checkpoint_ = nullptr;
        checkpoint();
        return true;
    }

private:
    TaskScope* scope_ = nullptr;
    runtime::Clock* clock_ = nullptr;
    double deadline_seconds_ = 0.0;
    double budget_seconds_ = 0.0;
    std::function<void()> checkpoint_;
    JobResult result_ = JobResult::Failed;
    bool finished_ = false;
};

class BackgroundJob {
public:
    virtual ~BackgroundJob() = default;

    // Stable across launches: the OS scheduler keys its entry by it.
    [[nodiscard]] virtual std::string_view id() const = 0;
    [[nodiscard]] virtual JobConstraints constraints() const = 0;
    // Spawns the work into `context.scope()` and returns; it must not block. False refuses the run.
    // The work ends by calling `context.finish(...)`.
    virtual bool start(JobContext& context) = 0;
};

// The OS scheduler seam. Both calls are requests: the OS decides when (and whether) to wake the
// app, which is why there is no "run now" here.
class JobScheduler {
public:
    virtual ~JobScheduler() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    // Registers or replaces the entry for `job_id`. False when the platform refused it.
    virtual bool schedule(std::string_view job_id, const JobConstraints& constraints) = 0;
    virtual bool cancel(std::string_view job_id) = 0;
};

} // namespace tessera::appfw
