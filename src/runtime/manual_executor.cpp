// SPDX-License-Identifier: MIT
//
// The deterministic manual executor: the same task shape as `CompletionQueue`, but drained only
// when a test calls `run`/`run_one`, with no thread and no wake.

#include "tessera/runtime/async.hpp"

#include <stdexcept>
#include <utility>

namespace tessera::runtime {

ManualExecutor::ManualExecutor(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("manual executor capacity must be greater than zero");
    }
}

bool ManualExecutor::post(std::function<void()> task, CancellationToken token) {
    if (!task) {
        ++stats_.dropped;
        return false;
    }
    if (token.cancelled()) {
        ++stats_.cancelled;
        return false;
    }
    if (tasks_.size() >= capacity_) {
        ++stats_.dropped;
        return false;
    }

    tasks_.push_back(Task{std::move(task), std::move(token)});
    ++stats_.posted;
    return true;
}

bool ManualExecutor::run_one() {
    while (!tasks_.empty()) {
        Task task = std::move(tasks_.front());
        tasks_.pop_front();

        if (task.token.cancelled()) {
            ++stats_.cancelled;
            continue;
        }

        ++stats_.executed;
        task.function();
        return true;
    }
    return false;
}

std::size_t ManualExecutor::run(std::size_t budget) {
    std::size_t executed = 0;
    while (executed < budget && run_one()) {
        ++executed;
    }
    return executed;
}

std::size_t ManualExecutor::clear() {
    const std::size_t discarded = tasks_.size();
    stats_.discarded += discarded;
    tasks_.clear();
    return discarded;
}

ManualExecutor::Stats ManualExecutor::stats() const {
    Stats result = stats_;
    result.pending = tasks_.size();
    return result;
}
} // namespace tessera::runtime
