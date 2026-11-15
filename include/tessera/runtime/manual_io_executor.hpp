// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/io_task_executor.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <utility>

namespace tessera::runtime {

// Deterministic `IoTaskExecutor` for tests: a bounded FIFO of I/O work and a bounded FIFO of
// completions, both drained by the test with no thread, no sleep and no wall clock.
//
// It mirrors `ManualExecutor`, with one deliberate difference: `ManualExecutor` holds a single FIFO
// of closures, so a test cannot tell "the I/O body ran" from "the completion ran" the way the real
// executor's two-stage pipeline separates them. Here the stages are separate queues:
//
//   * `run_one()` runs one I/O body (the blocking half); only then is its completion enqueued;
//   * `run_completions()` runs the completions whose bodies already ran (the UI-thread half).
//
// A completion therefore cannot run before its body, exactly as on the real executor, and a test
// can assert what the UI thread has and has not observed at each step.
class ManualIoExecutor final : public IoTaskExecutor {
public:
    struct Stats {
        std::size_t posted = 0;
        std::size_t executed = 0;
        std::size_t failed = 0;
        std::size_t rejected = 0;
        std::size_t completions = 0;
    };

    explicit ManualIoExecutor(std::size_t capacity = 64);

    ManualIoExecutor(const ManualIoExecutor&) = delete;
    ManualIoExecutor& operator=(const ManualIoExecutor&) = delete;

    bool post(IoTask task) override;

    [[nodiscard]] std::size_t capacity() const noexcept override;

    // Runs one queued I/O body, then enqueues its completion. False when the work queue is empty. A
    // body that throws returns true and is counted as failed: an exception never escapes the
    // executor boundary (ADR-025).
    bool run_one();

    // Runs one enqueued completion. False when there is none.
    bool run_completion_one();

    // Runs up to `budget` bodies, stopping earlier when the queue drains.
    std::size_t run(std::size_t budget);
    // Runs up to `budget` completions, stopping earlier when the queue drains.
    std::size_t run_completions(std::size_t budget);

    // Drops queued work and completions. A dropped body never runs, so a test can prove that work
    // abandoned while queued does nothing at all.
    std::size_t clear();

    [[nodiscard]] std::size_t pending_work() const noexcept {
        return work_.size();
    }
    [[nodiscard]] std::size_t pending_completions() const noexcept {
        return completions_.size();
    }
    [[nodiscard]] Stats stats() const noexcept {
        return stats_;
    }

private:
    struct Pending {
        std::function<void()> work;
        std::function<void()> completion;
    };

    std::deque<Pending> work_;
    std::deque<std::function<void()>> completions_;
    std::size_t capacity_ = 0;
    Stats stats_;
};

} // namespace tessera::runtime
