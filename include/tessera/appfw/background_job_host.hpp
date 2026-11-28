// SPDX-License-Identifier: MIT
//
// `HeadlessJobHost`: the headless `Application` entry for one OS-woken background job (issue
// #1043; docs/design/background-jobs.md). The same `Application` and `ServiceRunner` the visible
// app uses, with no window: it launches the declared services, runs the job in a `TaskScope`
// under a hard budget, then closes the scope and stops the services in reverse order.
//
// Phases (one `tick()` advances at most one step; nothing blocks, nothing sleeps):
//
//   Starting -> Running -> Closing -> Stopping -> Done
//
//   * Starting: waits for the start sequence. A required-service failure ends the run as
//     `NotStarted` without running the job;
//   * Running: the job's work is on the completion queue. It ends when the job calls `finish`, when
//     the clock reaches the budget (`BudgetExpired`) or when the OS expiration handler calls
//     `request_stop` (`Stopped`). The latter two run the checkpoint callback first;
//   * Closing: the job scope is closing under a bounded grace deadline (TaskScope rules);
//   * Stopping: the application's termination gate runs the service stop sequence.
//
// The budget runs from `begin`, so service start-up is charged to it, as it is on a real OS. The
// host never drains the completion queue: the caller's loop does, exactly as for `RuntimeLoop`.

#pragma once

#include "tessera/appfw/application.hpp"
#include "tessera/appfw/background_job.hpp"
#include "tessera/appfw/task_scope.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace tessera::appfw {

struct HeadlessJobConfig {
    // Subtracted from the OS budget so the checkpoint and the close finish inside it.
    double budget_margin_seconds = 2.0;
    // How long a closing scope may take to drain before the host gives up waiting on it.
    double close_grace_seconds = 0.5;
    std::size_t scope_capacity = 16;
};

enum class JobHostPhase : std::uint8_t { Idle, Starting, Running, Closing, Stopping, Done };

[[nodiscard]] constexpr const char* to_string(JobHostPhase phase) noexcept {
    switch (phase) {
    case JobHostPhase::Idle:
        return "idle";
    case JobHostPhase::Starting:
        return "starting";
    case JobHostPhase::Running:
        return "running";
    case JobHostPhase::Closing:
        return "closing";
    case JobHostPhase::Stopping:
        return "stopping";
    case JobHostPhase::Done:
        return "done";
    }
    return "idle";
}

class HeadlessJobHost {
public:
    // All borrowed and must outlive the host. `runner` must be prepared (an empty one is fine).
    HeadlessJobHost(Application& application, runtime::Clock& clock,
                    runtime::CompletionQueue& completions, HeadlessJobConfig config = {});

    HeadlessJobHost(const HeadlessJobHost&) = delete;
    HeadlessJobHost& operator=(const HeadlessJobHost&) = delete;

    // Composition root, before `begin`. Borrowed. False for a null job, an empty or duplicate id.
    bool add(BackgroundJob& job);

    // The OS woke the process for `job_id` with `os_budget_seconds` to spare. False when the host
    // is not idle, the id is unknown, or the application could not begin launching.
    bool begin(std::string_view job_id, double os_budget_seconds);

    // The OS expiration handler: checkpoint, cancel, close. A no-op unless `Running`.
    void request_stop();

    void tick();

    [[nodiscard]] JobHostPhase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] JobOutcome outcome() const noexcept {
        return outcome_;
    }
    [[nodiscard]] bool done() const noexcept {
        return phase_ == JobHostPhase::Done;
    }
    [[nodiscard]] bool checkpoint_ran() const noexcept {
        return checkpoint_ran_;
    }
    // True when the job scope was still not drained after the close grace.
    [[nodiscard]] bool scope_timed_out() const noexcept {
        return scope_timed_out_;
    }
    [[nodiscard]] double budget_seconds() const noexcept {
        return budget_seconds_;
    }

private:
    void start_job();
    void expire(JobOutcome outcome);
    void begin_closing();

    Application* application_ = nullptr;
    runtime::Clock* clock_ = nullptr;
    runtime::CompletionQueue* completions_ = nullptr;
    HeadlessJobConfig config_;
    std::vector<BackgroundJob*> jobs_;
    BackgroundJob* active_ = nullptr;
    std::unique_ptr<TaskScope> scope_;
    std::unique_ptr<JobContext> context_;
    JobHostPhase phase_ = JobHostPhase::Idle;
    JobOutcome outcome_ = JobOutcome::Pending;
    double deadline_seconds_ = 0.0;
    double budget_seconds_ = 0.0;
    bool checkpoint_ran_ = false;
    bool scope_timed_out_ = false;
    bool termination_requested_ = false;
};

} // namespace tessera::appfw
