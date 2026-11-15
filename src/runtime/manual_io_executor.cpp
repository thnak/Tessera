// SPDX-License-Identifier: MIT

#include "tessera/runtime/manual_io_executor.hpp"

#include <stdexcept>

namespace tessera::runtime {

ManualIoExecutor::ManualIoExecutor(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("manual io executor capacity must be greater than zero");
    }
}

std::size_t ManualIoExecutor::capacity() const noexcept {
    return capacity_;
}

bool ManualIoExecutor::post(IoTask task) {
    if (!task.valid()) {
        ++stats_.rejected;
        return false;
    }
    if (work_.size() >= capacity_) {
        ++stats_.rejected;
        return false;
    }
    work_.push_back(Pending{std::move(task.work), std::move(task.completion)});
    ++stats_.posted;
    return true;
}

bool ManualIoExecutor::run_one() {
    if (work_.empty()) {
        return false;
    }
    Pending pending = std::move(work_.front());
    work_.pop_front();
    bool succeeded = true;
    try {
        pending.work();
    } catch (...) {
        succeeded = false;
    }
    if (succeeded) {
        ++stats_.executed;
    } else {
        ++stats_.failed;
    }
    // The completion exists only after the body ran, whichever way it ended; a failed body's
    // completion is what reports the failure to the UI thread.
    if (pending.completion) {
        completions_.push_back(std::move(pending.completion));
    }
    return true;
}

bool ManualIoExecutor::run_completion_one() {
    if (completions_.empty()) {
        return false;
    }
    std::function<void()> completion = std::move(completions_.front());
    completions_.pop_front();
    ++stats_.completions;
    completion();
    return true;
}

std::size_t ManualIoExecutor::run(std::size_t budget) {
    std::size_t executed = 0;
    while (executed < budget && run_one()) {
        ++executed;
    }
    return executed;
}

std::size_t ManualIoExecutor::run_completions(std::size_t budget) {
    std::size_t executed = 0;
    while (executed < budget && run_completion_one()) {
        ++executed;
    }
    return executed;
}

std::size_t ManualIoExecutor::clear() {
    const std::size_t discarded = work_.size() + completions_.size();
    work_.clear();
    completions_.clear();
    return discarded;
}

} // namespace tessera::runtime
