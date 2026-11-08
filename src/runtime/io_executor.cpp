// SPDX-License-Identifier: MIT

#include "tessera/runtime/io_executor.hpp"
#include "tessera/runtime/worker_executor.hpp"

#include <utility>

namespace tessera::runtime {
namespace {

void update_peak(std::atomic<std::size_t>& peak, std::size_t value) {
    std::size_t current = peak.load(std::memory_order_relaxed);
    while (current < value) {
        if (peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
            return;
        }
    }
}

} // namespace

IoExecutor::IoExecutor(WorkerExecutor& workers, CompletionQueue& completions)
    : IoExecutor(workers, completions, Config{}) {}

IoExecutor::IoExecutor(WorkerExecutor& workers, CompletionQueue& completions, Config config)
    : workers_(&workers),
      completions_(&completions),
      config_(config) {
    if (config_.max_in_flight == 0) {
        config_.max_in_flight = 1;
    }
}

bool IoExecutor::reserve_slot() {
    std::size_t current = in_flight_.load(std::memory_order_relaxed);
    while (current < config_.max_in_flight) {
        if (in_flight_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel)) {
            update_peak(peak_in_flight_, current + 1);
            return true;
        }
    }
    return false;
}

void IoExecutor::release_slot() {
    in_flight_.fetch_sub(1, std::memory_order_acq_rel);
}

bool IoExecutor::submit(std::function<void()> job, std::function<void()> completion,
                        CompletionPriority priority, CancellationToken token) {
    if (!job) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (token.cancelled()) {
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!reserve_slot()) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    submitted_.fetch_add(1, std::memory_order_relaxed);

    // The worker gets no cancellation token: the wrapper must always run so the in-flight slot
    // is released, even when the caller cancels while the job is queued.
    const bool posted =
        workers_->post([this, job = std::move(job), completion = std::move(completion), priority,
                        token]() mutable {
            if (token.cancelled()) {
                cancelled_.fetch_add(1, std::memory_order_relaxed);
            } else {
                try {
                    job();
                    completed_.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    failed_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            release_slot();

            if (completion && !token.cancelled()) {
                completions_->post(priority, std::move(completion), token);
            }
        });

    if (!posted) {
        release_slot();
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

IoExecutor::Stats IoExecutor::stats() const {
    Stats result;
    result.submitted = submitted_.load(std::memory_order_relaxed);
    result.completed = completed_.load(std::memory_order_relaxed);
    result.failed = failed_.load(std::memory_order_relaxed);
    result.cancelled = cancelled_.load(std::memory_order_relaxed);
    result.rejected = rejected_.load(std::memory_order_relaxed);
    result.in_flight = in_flight_.load(std::memory_order_relaxed);
    result.peak_in_flight = peak_in_flight_.load(std::memory_order_relaxed);
    return result;
}

} // namespace tessera::runtime
