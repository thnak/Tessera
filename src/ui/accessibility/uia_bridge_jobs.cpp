// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_bridge.hpp"

#include <algorithm>
#include <chrono>

namespace tessera::ui::a11y {

std::expected<void, UiaError> UiaBridge::submit(const ProviderFn& fn, int wait_ms) {
    bool inline_call = false;
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_ || worker_done_ || !thread_.joinable()) {
            const UiaError failure =
                phase() == UiaBridgePhase::Failed ? error() : UiaError::NotStarted;
            return std::unexpected(failure);
        }
        // A COM callback that arrives on the owning thread must not queue behind itself.
        inline_call = bridge_thread_ == std::this_thread::get_id();
    }
    if (inline_call) {
        try {
            if (fn) {
                fn(provider_);
            }
        } catch (...) {
            // Boundary: never let an exception leave the bridge/FFI seam (ADR-025).
            return std::unexpected(UiaError::Internal);
        }
        return {};
    }

    std::shared_ptr<Job> job;
    try {
        job = std::make_shared<Job>();
        job->fn = fn;
    } catch (...) {
        return std::unexpected(UiaError::OutOfMemory);
    }
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_ || worker_done_) {
            const UiaError failure =
                phase() == UiaBridgePhase::Failed ? error() : UiaError::NotStarted;
            return std::unexpected(failure);
        }
        if (job_count_ >= jobs_.size()) {
            return std::unexpected(UiaError::QueueFull);
        }
        job->ticket = ++next_ticket_;
        jobs_[(job_head_ + job_count_) % jobs_.size()] = job;
        ++job_count_;
    }
    cv_.notify_all();

    std::unique_lock lock(mutex_);
    const auto bound = std::chrono::milliseconds(std::max(wait_ms, 0));
    const bool completed =
        cv_.wait_for(lock, bound, [this, &job] { return job->done || worker_done_; });
    if (!completed) {
        // Not yet started: make sure it never runs, so the caller's captures can die safely. A job
        // that is already running is bounded provider work, so wait for it instead of returning
        // while it may still touch the caller's data.
        if (!job->running) {
            job->abandoned = true;
            return std::unexpected(UiaError::Timeout);
        }
        cv_.wait(lock, [this, &job] { return job->done || worker_done_; });
    }
    if (job->done) {
        if (job->error != UiaError::None) {
            return std::unexpected(job->error);
        }
        return {};
    }
    const UiaError failure = phase() == UiaBridgePhase::Failed ? error() : UiaError::NotStarted;
    return std::unexpected(failure);
}

std::expected<void, UiaError> UiaBridge::run_on_bridge(const ProviderFn& fn, int wait_ms) {
    // `submit` already runs inline when the caller is the bridge thread, so a COM callback that
    // arrives on the owning thread never queues behind itself.
    return submit(fn, wait_ms);
}

std::expected<void, UiaError> UiaBridge::post(ProviderFn fn) {
    if (!fn) {
        return {};
    }
    bool inline_call = false;
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_ || worker_done_ || !thread_.joinable()) {
            const UiaError failure =
                phase() == UiaBridgePhase::Failed ? error() : UiaError::NotStarted;
            return std::unexpected(failure);
        }
        inline_call = bridge_thread_ == std::this_thread::get_id();
    }
    if (inline_call) {
        try {
            fn(provider_);
        } catch (...) {
            return std::unexpected(UiaError::Internal);
        }
        return {};
    }

    std::shared_ptr<Job> job;
    try {
        job = std::make_shared<Job>();
        job->fn = std::move(fn);
    } catch (...) {
        return std::unexpected(UiaError::OutOfMemory);
    }
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_ || worker_done_) {
            const UiaError failure =
                phase() == UiaBridgePhase::Failed ? error() : UiaError::NotStarted;
            return std::unexpected(failure);
        }
        if (job_count_ >= jobs_.size()) {
            return std::unexpected(UiaError::QueueFull);
        }
        job->ticket = ++next_ticket_;
        jobs_[(job_head_ + job_count_) % jobs_.size()] = job;
        ++job_count_;
    }
    cv_.notify_all();
    return {};
}

std::expected<void, UiaError> UiaBridge::step(int wait_ms) {
    const auto result = submit(ProviderFn{}, wait_ms);
    if (!result) {
        return result;
    }
    if (phase() == UiaBridgePhase::Failed) {
        return std::unexpected(error());
    }
    return {};
}

void UiaBridge::run_jobs() {
    const std::size_t budget = std::max<std::size_t>(config_.max_jobs_per_iteration, 1);
    for (std::size_t ran = 0; ran < budget; ++ran) {
        std::shared_ptr<Job> job;
        bool skipped = false;
        {
            std::lock_guard lock(mutex_);
            if (job_count_ == 0 || stop_requested_) {
                return;
            }
            job = std::move(jobs_[job_head_]);
            job_head_ = (job_head_ + 1) % jobs_.size();
            --job_count_;
            if (job && job->abandoned) {
                job->done = true;
                job->error = UiaError::Timeout;
                skipped = true;
            } else if (job) {
                job->running = true;
            }
        }
        if (skipped) {
            cv_.notify_all();
            continue;
        }
        if (!job) {
            continue;
        }
        // Adopt immediately before the job runs, not once per loop iteration: a snapshot published
        // while this batch is in flight must still be visible to the job (and to the events it
        // raises), and `step` must be a real adoption barrier.
        adopt_pending_snapshot();
        UiaError result = UiaError::None;
        try {
            if (job->fn) {
                job->fn(provider_);
            }
        } catch (...) {
            // Boundary: a host callback that throws must not unwind into the loop or the client.
            result = UiaError::Internal;
        }
        {
            std::lock_guard lock(mutex_);
            job->done = true;
            job->error = result;
        }
        cv_.notify_all();
    }
}

void UiaBridge::finish_worker(UiaError failure) {
    {
        std::lock_guard lock(mutex_);
        if (failure != UiaError::None) {
            phase_.store(UiaBridgePhase::Failed);
            error_.store(failure);
        }
        // Wake every waiter: a stopped worker will never run its pending jobs.
        for (std::size_t offset = 0; offset < job_count_; ++offset) {
            auto& job = jobs_[(job_head_ + offset) % jobs_.size()];
            if (job) {
                job->done = true;
                job->error = failure == UiaError::None ? UiaError::NotStarted : failure;
            }
            job.reset();
        }
        job_head_ = 0;
        job_count_ = 0;
        bridge_thread_ = std::thread::id{};
        worker_done_ = true;
        alive_.store(false, std::memory_order_release);
    }
    // The provider keeps its dead owner thread so a stray call after shutdown is rejected instead
    // of becoming an unsynchronized read of the last snapshot.
    cv_.notify_all();
}

} // namespace tessera::ui::a11y
