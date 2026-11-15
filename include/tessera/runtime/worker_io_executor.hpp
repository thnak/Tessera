// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/io_task_executor.hpp"
#include "tessera/runtime/worker_executor.hpp"

#include <atomic>
#include <cstddef>

namespace tessera::runtime {

// Production `IoTaskExecutor`: a bounded front end over one owned `WorkerExecutor` pool, with the
// completion posted back to the shared `CompletionQueue` the UI thread drains.
//
// It reuses the layers that already exist instead of adding a second pool: `WorkerExecutor` owns
// and joins its threads (ADR-021: no detached tasks), and `CompletionQueue` is the single path a
// worker uses to touch state the UI thread owns. What this class adds is the *I/O* discipline on
// top of them, which is the part AF7 is responsible for:
//
//   * at most `Config::max_in_flight` I/O jobs are queued or running at once, so a slow disk cannot
//     fill the worker queue and starve unrelated work. At the limit, `post()` returns false and
//     counts a rejection; it never blocks and never runs the job inline;
//   * a slot is released as soon as the I/O body finishes, before the completion is posted, so a
//     completion that re-submits I/O cannot deadlock against the bound;
//   * an exception from the I/O body is caught here and counted as `failed` (ADR-025: an exception
//     never crosses a thread boundary), and the slot is still released;
//   * shutdown() stops accepting work, drains what is queued, and joins the threads. The destructor
//     does the same.
//
// Threading: `post()` may be called from the UI thread; `stats()` may be read from any thread.
// Completions run on whichever thread drains the queue (the UI thread in the app), never on the
// worker's stack.
class WorkerIoExecutor final : public IoTaskExecutor {
public:
    struct Config {
        // Queue+run bound. Must be at least 1; 0 is clamped to 1.
        std::size_t max_in_flight = 4;
        // Worker threads that execute the blocking calls. Must be at least 1; 0 is clamped to 1.
        std::size_t workers = 2;
        // Handoff capacity per worker; clamped up to `max_in_flight` so a reserved slot always has
        // somewhere to go.
        std::size_t queue_capacity = 64;
        CompletionPriority priority = CompletionPriority::Visible;
    };

    struct Stats {
        std::size_t posted = 0;
        std::size_t started = 0;
        std::size_t completed = 0;
        std::size_t failed = 0;
        std::size_t rejected = 0;
        std::size_t in_flight = 0;
        std::size_t peak_in_flight = 0;
        // Completions the queue refused (a full `CompletionQueue`). The I/O body already ran, so
        // the module that submitted the task is responsible for its own state in that case.
        std::size_t completions_dropped = 0;
    };

    explicit WorkerIoExecutor(CompletionQueue& completions);
    WorkerIoExecutor(CompletionQueue& completions, Config config);
    ~WorkerIoExecutor() override;

    WorkerIoExecutor(const WorkerIoExecutor&) = delete;
    WorkerIoExecutor& operator=(const WorkerIoExecutor&) = delete;

    // Enqueues `task.work` for a worker and `task.completion` for the completion queue. Returns
    // false for an invalid task, when shutting down, or at the in-flight bound.
    bool post(IoTask task) override;

    [[nodiscard]] std::size_t capacity() const noexcept override;

    // Stops accepting work, drains the queued jobs, joins the workers. Idempotent.
    void shutdown();

    [[nodiscard]] Stats stats() const;

private:
    bool reserve_slot();
    void release_slot();

    WorkerExecutor workers_;
    CompletionQueue* completions_ = nullptr;
    Config config_;
    std::atomic<std::size_t> posted_{0};
    std::atomic<std::size_t> started_{0};
    std::atomic<std::size_t> completed_{0};
    std::atomic<std::size_t> failed_{0};
    std::atomic<std::size_t> rejected_{0};
    std::atomic<std::size_t> in_flight_{0};
    std::atomic<std::size_t> peak_in_flight_{0};
    std::atomic<std::size_t> completions_dropped_{0};
    std::atomic<bool> stopping_{false};
};

} // namespace tessera::runtime
