// SPDX-License-Identifier: MIT

#include "tessera/runtime/worker_io_executor.hpp"

#include <algorithm>
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

WorkerIoExecutor::WorkerIoExecutor(CompletionQueue& completions)
    : WorkerIoExecutor(completions, Config{}) {}

WorkerIoExecutor::WorkerIoExecutor(CompletionQueue& completions, Config config)
    : workers_(completions,
               WorkerExecutor::Config{
                   std::max<std::size_t>(1, config.workers),
                   std::max<std::size_t>(1, config.queue_capacity),
               }),
      completions_(&completions),
      config_(config) {
    if (config_.max_in_flight == 0) {
        config_.max_in_flight = 1;
    }
}

WorkerIoExecutor::~WorkerIoExecutor() {
    shutdown();
}

std::size_t WorkerIoExecutor::capacity() const noexcept {
    return config_.max_in_flight;
}

bool WorkerIoExecutor::reserve_slot() {
    std::size_t current = in_flight_.load(std::memory_order_relaxed);
    while (current < config_.max_in_flight) {
        if (in_flight_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel)) {
            update_peak(peak_in_flight_, current + 1);
            return true;
        }
    }
    return false;
}

void WorkerIoExecutor::release_slot() {
    in_flight_.fetch_sub(1, std::memory_order_acq_rel);
}

bool WorkerIoExecutor::post(IoTask task) {
    if (!task.valid()) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!reserve_slot()) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // A refused handoff is not an I/O failure: the job never ran and the slot goes straight back.
    const bool accepted = workers_.post(
        [this, work = std::move(task.work), completion = std::move(task.completion)]() mutable {
            started_.fetch_add(1, std::memory_order_relaxed);
            try {
                work();
                completed_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                failed_.fetch_add(1, std::memory_order_relaxed);
            }
            // Released before the completion is posted: a completion that submits more I/O must not
            // find the bound already exhausted by its own predecessor.
            release_slot();

            if (completion) {
                const bool delivered = completions_->post(config_.priority, std::move(completion));
                if (!delivered) {
                    completions_dropped_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

    if (!accepted) {
        release_slot();
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    posted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void WorkerIoExecutor::shutdown() {
    stopping_.store(true, std::memory_order_release);
    workers_.shutdown();
}

WorkerIoExecutor::Stats WorkerIoExecutor::stats() const {
    Stats result;
    result.posted = posted_.load(std::memory_order_relaxed);
    result.started = started_.load(std::memory_order_relaxed);
    result.completed = completed_.load(std::memory_order_relaxed);
    result.failed = failed_.load(std::memory_order_relaxed);
    result.rejected = rejected_.load(std::memory_order_relaxed);
    result.in_flight = in_flight_.load(std::memory_order_relaxed);
    result.peak_in_flight = peak_in_flight_.load(std::memory_order_relaxed);
    result.completions_dropped = completions_dropped_.load(std::memory_order_relaxed);
    return result;
}

} // namespace tessera::runtime
