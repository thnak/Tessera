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

void WorkerIoExecutor::release_slot() {
    shared_.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

bool WorkerIoExecutor::post(IoTask task) {
    if (!task.valid()) {
        ui_.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (ui_.stopping.load(std::memory_order_acquire)) {
        ui_.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!reserve_slot()) {
        ui_.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // A refused handoff is not an I/O failure: the job never ran and the slot goes straight back.
    const bool accepted = workers_.post(
        [this, work = std::move(task.work), completion = std::move(task.completion)]() mutable {
            worker_.started.fetch_add(1, std::memory_order_relaxed);
            try {
                work();
                worker_.completed.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                worker_.failed.fetch_add(1, std::memory_order_relaxed);
            }
            // Released before the completion is posted: a completion that submits more I/O must not
            // find the bound already exhausted by its own predecessor.
            release_slot();

            if (completion) {
                const bool delivered = completions_->post(config_.priority, std::move(completion));
                if (!delivered) {
                    worker_.completions_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

    if (!accepted) {
        release_slot();
        ui_.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ui_.posted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void WorkerIoExecutor::shutdown() {
    ui_.stopping.store(true, std::memory_order_release);
    workers_.shutdown();
}

WorkerIoExecutor::Stats WorkerIoExecutor::stats() const {
    Stats result;
    result.posted = ui_.posted.load(std::memory_order_relaxed);
    result.started = worker_.started.load(std::memory_order_relaxed);
    result.completed = worker_.completed.load(std::memory_order_relaxed);
    result.failed = worker_.failed.load(std::memory_order_relaxed);
    result.rejected = ui_.rejected.load(std::memory_order_relaxed);
    result.in_flight = shared_.in_flight.load(std::memory_order_relaxed);
    result.peak_in_flight = ui_.peak_in_flight.load(std::memory_order_relaxed);
    result.completions_dropped = worker_.completions_dropped.load(std::memory_order_relaxed);
    return result;
}

} // namespace tessera::runtime
