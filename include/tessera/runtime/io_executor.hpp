// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/cache_line.hpp"
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

    // Split by writer into cache-line groups, for the same reason as `WorkerIoExecutor` (security
    // review finding C1): `submitted_`/`rejected_`/`peak_in_flight_` are written by the submitting
    // (UI) thread, `completed_`/`failed_`/`cancelled_` by the workers, and `in_flight_` by both.
    // Declared consecutively they shared one line, so every submission and completion invalidated
    // the same line and the relaxed atomics serialized on it.
    struct UiCounters {
        std::atomic<std::size_t> submitted{0};
        std::atomic<std::size_t> rejected{0};
        std::atomic<std::size_t> peak_in_flight{0};
    };
    struct WorkerCounters {
        std::atomic<std::size_t> completed{0};
        std::atomic<std::size_t> failed{0};
        std::atomic<std::size_t> cancelled{0};
    };
    // Written by both threads, so it gets a line of its own: its CAS traffic must not invalidate
    // the counters on either side.
    struct SharedCounters {
        std::atomic<std::size_t> in_flight{0};
    };

    WorkerExecutor* workers_ = nullptr;
    CompletionQueue* completions_ = nullptr;
    Config config_;
    // The groups are cache-line aligned, so the executor itself is padded to a line boundary; that
    // padding is the point (see tessera/detail/cache_line.hpp).
    TESSERA_CACHE_LINE_BEGIN
    ::tessera::detail::CacheLine<UiCounters> ui_;
    ::tessera::detail::CacheLine<WorkerCounters> worker_;
    ::tessera::detail::CacheLine<SharedCounters> shared_;
    TESSERA_CACHE_LINE_END
};

} // namespace tessera::runtime
