// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/async.hpp"

#include <atomic>
#include <cstddef>
#include <functional>

namespace tessera::runtime {

class WorkerExecutor;

// Bounded front end for blocking I/O work on top of WorkerExecutor. The UI thread submits a job
// and a completion; the job runs on a worker and the completion is posted back to the shared
// CompletionQueue. At most `max_in_flight` jobs may be running or queued, so a slow disk cannot
// fill the worker queue and starve other work. The executor does not own the worker pool.
class IoExecutor {
public:
    struct Config {
        std::size_t max_in_flight = 4;
    };

    struct Stats {
        std::size_t submitted = 0;
        std::size_t completed = 0;
        std::size_t failed = 0;
        std::size_t cancelled = 0;
        std::size_t rejected = 0;
        std::size_t in_flight = 0;
        std::size_t peak_in_flight = 0;
    };

    IoExecutor(WorkerExecutor& workers, CompletionQueue& completions);
    IoExecutor(WorkerExecutor& workers, CompletionQueue& completions, Config config);

    IoExecutor(const IoExecutor&) = delete;
    IoExecutor& operator=(const IoExecutor&) = delete;

    // Schedules `job` for a worker and `completion` for the queue at `priority`. Returns false
    // for an empty job, an already-cancelled token, or when max_in_flight is reached. A job must
    // not throw; exceptions are counted as failed and still release the slot. Cancellation is
    // observed before the job starts; a running blocking call is not interrupted.
    bool submit(std::function<void()> job, std::function<void()> completion = {},
                CompletionPriority priority = CompletionPriority::Visible,
                CancellationToken token = {});

    [[nodiscard]] Stats stats() const;

private:
    bool reserve_slot();
    void release_slot();

    WorkerExecutor* workers_ = nullptr;
    CompletionQueue* completions_ = nullptr;
    Config config_;
    std::atomic<std::size_t> submitted_{0};
    std::atomic<std::size_t> completed_{0};
    std::atomic<std::size_t> failed_{0};
    std::atomic<std::size_t> cancelled_{0};
    std::atomic<std::size_t> rejected_{0};
    std::atomic<std::size_t> in_flight_{0};
    std::atomic<std::size_t> peak_in_flight_{0};
};

} // namespace tessera::runtime
