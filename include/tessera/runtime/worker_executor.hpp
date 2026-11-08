// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/async.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tessera::runtime {

// Bounded pool of owned worker threads. Tasks run off the UI thread and usually post their
// result back through the shared CompletionQueue (the priority overload does that for you).
// shutdown() stops accepting work, drains already-queued tasks, and joins the workers; it never
// detaches a thread.
class WorkerExecutor {
public:
    struct Config {
        std::size_t workers = 1;
        std::size_t capacity = 256;
    };

    struct Stats {
        std::size_t posted = 0;
        std::size_t executed = 0;
        std::size_t cancelled = 0;
        std::size_t dropped = 0;
        std::size_t pending = 0;
        std::size_t active = 0;
    };

    explicit WorkerExecutor(CompletionQueue& completions, Config config = Config{});
    ~WorkerExecutor();

    WorkerExecutor(const WorkerExecutor&) = delete;
    WorkerExecutor& operator=(const WorkerExecutor&) = delete;

    // Runs `task` on a worker. Returns false for an empty/cancelled task or when stopping/full.
    bool post(std::function<void()> task, CancellationToken token = {});

    // Wraps `task` so it runs on a worker and posts `completion` to the shared queue with
    // `priority`; the caller's cancellation token gates both the run and the completion.
    bool post(CompletionPriority priority, std::function<void()> completion,
              CancellationToken token = {});

    void shutdown();

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::size_t pending() const;

private:
    struct Task {
        std::function<void()> function;
        CancellationToken token;
    };

    void worker_loop();

    CompletionQueue* completions_ = nullptr;
    Config config_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable wake_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    Stats stats_;
};

} // namespace tessera::runtime
