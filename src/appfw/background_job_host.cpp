// SPDX-License-Identifier: MIT
//
// `HeadlessJobHost` (issue #1043). See include/tessera/appfw/background_job_host.hpp.

#include "tessera/appfw/background_job_host.hpp"

#include <algorithm>

namespace tessera::appfw {

HeadlessJobHost::HeadlessJobHost(Application& application, runtime::Clock& clock,
                                 runtime::CompletionQueue& completions, HeadlessJobConfig config)
    : application_(&application),
      clock_(&clock),
      completions_(&completions),
      config_(config) {}

bool HeadlessJobHost::add(BackgroundJob& job) {
    const std::string_view id = job.id();
    if (id.empty() || phase_ != JobHostPhase::Idle) {
        return false;
    }
    const bool duplicate = std::any_of(
        jobs_.begin(), jobs_.end(), [&](const BackgroundJob* other) { return other->id() == id; });
    if (duplicate) {
        return false;
    }
    jobs_.push_back(&job);
    return true;
}

bool HeadlessJobHost::begin(std::string_view job_id, double os_budget_seconds) {
    if (phase_ != JobHostPhase::Idle) {
        return false;
    }
    const auto found = std::find_if(jobs_.begin(), jobs_.end(),
                                    [&](const BackgroundJob* job) { return job->id() == job_id; });
    if (found == jobs_.end()) {
        return false;
    }
    active_ = *found;
    budget_seconds_ = os_budget_seconds - config_.budget_margin_seconds;
    if (budget_seconds_ <= 0.0) {
        // Nothing usable is left after the margin: do not launch services just to tear them down.
        budget_seconds_ = 0.0;
        outcome_ = JobOutcome::BudgetExpired;
        phase_ = JobHostPhase::Done;
        return true;
    }
    if (!application_->launch()) {
        active_ = nullptr;
        return false;
    }
    deadline_seconds_ = clock_->now_seconds() + budget_seconds_;
    phase_ = JobHostPhase::Starting;
    return true;
}

void HeadlessJobHost::start_job() {
    TaskScopeConfig scope_config;
    scope_config.capacity = config_.scope_capacity;
    scope_ = std::make_unique<TaskScope>(*clock_, *completions_, scope_config);
    context_ = std::make_unique<JobContext>(*scope_, *clock_, deadline_seconds_, budget_seconds_);
    if (!active_->start(*context_)) {
        outcome_ = JobOutcome::NotStarted;
        begin_closing();
        return;
    }
    phase_ = JobHostPhase::Running;
}

void HeadlessJobHost::begin_closing() {
    if (scope_ == nullptr) {
        phase_ = JobHostPhase::Stopping;
        return;
    }
    scope_->close(clock_->now_seconds() + config_.close_grace_seconds);
    phase_ = JobHostPhase::Closing;
}

void HeadlessJobHost::expire(JobOutcome outcome) {
    // The checkpoint runs before the cancel so the job can still read its own state.
    checkpoint_ran_ = context_->run_checkpoint();
    outcome_ = outcome;
    begin_closing();
}

void HeadlessJobHost::request_stop() {
    if (phase_ == JobHostPhase::Running) {
        expire(JobOutcome::Stopped);
    }
}

void HeadlessJobHost::tick() {
    switch (phase_) {
    case JobHostPhase::Idle:
    case JobHostPhase::Done:
        return;
    case JobHostPhase::Starting: {
        application_->tick();
        const ServicePhase services = application_->runner().phase();
        if (services == ServicePhase::Failed) {
            outcome_ = JobOutcome::NotStarted;
            phase_ = JobHostPhase::Stopping;
        } else if (clock_->now_seconds() >= deadline_seconds_) {
            outcome_ = JobOutcome::BudgetExpired;
            phase_ = JobHostPhase::Stopping;
        } else if (services == ServicePhase::Running) {
            start_job();
        }
        return;
    }
    case JobHostPhase::Running:
        if (context_->finished()) {
            switch (context_->result()) {
            case JobResult::Success:
                outcome_ = JobOutcome::Success;
                break;
            case JobResult::Retry:
                outcome_ = JobOutcome::RetryRequested;
                break;
            case JobResult::Failed:
                outcome_ = JobOutcome::Failed;
                break;
            }
            begin_closing();
        } else if (clock_->now_seconds() >= deadline_seconds_) {
            expire(JobOutcome::BudgetExpired);
        }
        return;
    case JobHostPhase::Closing: {
        const TaskScopeCloseState state = scope_->tick();
        if (state == TaskScopeCloseState::TimedOut) {
            scope_timed_out_ = true;
        }
        if (state == TaskScopeCloseState::Closed || state == TaskScopeCloseState::TimedOut) {
            phase_ = JobHostPhase::Stopping;
        }
        return;
    }
    case JobHostPhase::Stopping:
        if (!termination_requested_) {
            // A service that defers keeps the request open; it is retried on the next tick and the
            // OS kill at the real budget is the backstop, as for any process.
            termination_requested_ =
                application_->request_termination() != TerminationOutcome::Deferred;
        }
        application_->tick();
        if (application_->state() == AppState::Terminated) {
            phase_ = JobHostPhase::Done;
        }
        return;
    }
}

} // namespace tessera::appfw
