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
    std::size_t current = shared_.in_flight.load(std::memory_order_relaxed);
    while (current < config_.max_in_flight) {
        if (shared_.in_flight.compare_exchange_weak(current, current + 1,
                                                    std::memory_order_acq_rel)) {
            update_peak(ui_.peak_in_flight, current + 1);
            return true;
        }
    }
    return false;
}

void IoExecutor::release_slot() {
    shared_.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

bool IoExecutor::submit(std::function<void()> job, std::function<void()> completion,
                        CompletionPriority priority, CancellationToken token) {
    if (!job) {
        ui_.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (token.cancelled()) {
        worker_.cancelled.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!reserve_slot()) {
        ui_.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ui_.submitted.fetch_add(1, std::memory_order_relaxed);

    // The worker gets no cancellation token: the wrapper must always run so the in-flight slot
    // is released, even when the caller cancels while the job is queued.
    const bool posted =
        workers_->post([this, job = std::move(job), completion = std::move(completion), priority,
                        token]() mutable {
            if (token.cancelled()) {
                worker_.cancelled.fetch_add(1, std::memory_order_relaxed);
            } else {
                try {
                    job();
                    worker_.completed.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    worker_.failed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            release_slot();

            if (completion && !token.cancelled()) {
                completions_->post(priority, std::move(completion), token);
            }
        });

    if (!posted) {
        release_slot();
        ui_.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

IoExecutor::Stats IoExecutor::stats() const {
    Stats result;
    result.submitted = ui_.submitted.load(std::memory_order_relaxed);
    result.completed = worker_.completed.load(std::memory_order_relaxed);
    result.failed = worker_.failed.load(std::memory_order_relaxed);
    result.cancelled = worker_.cancelled.load(std::memory_order_relaxed);
    result.rejected = ui_.rejected.load(std::memory_order_relaxed);
    result.in_flight = shared_.in_flight.load(std::memory_order_relaxed);
    result.peak_in_flight = ui_.peak_in_flight.load(std::memory_order_relaxed);
    return result;
}

} // namespace tessera::runtime
