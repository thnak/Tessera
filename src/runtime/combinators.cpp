// SPDX-License-Identifier: MIT
//
// The non-template half of the task combinators (issue #844): the aggregate error type and the
// exception_ptr inspection both awaiters need.

#include "tessera/runtime/combinators.hpp"

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace tessera::runtime {

TaskAggregateError::TaskAggregateError(std::vector<std::exception_ptr> failures)
    : failures_(std::move(failures)) {
    for (std::size_t index = 0; index < failures_.size(); ++index) {
        if (failures_[index] != nullptr) {
            first_index_ = index;
            break;
        }
    }
    // The message names the failure count and the first failing index, so a log line from
    // `what()` alone is already actionable.
    std::size_t failed = 0;
    for (const std::exception_ptr& failure : failures_) {
        if (failure != nullptr) {
            ++failed;
        }
    }
    message_ = "when_all: " + std::to_string(failed) + " of " + std::to_string(failures_.size()) +
               " children failed (first at index " + std::to_string(first_index_) + ")";
}

const char* TaskAggregateError::what() const noexcept {
    return message_.c_str();
}

const std::exception_ptr& TaskAggregateError::first() const noexcept {
    return failures_[first_index_];
}

namespace detail {

bool is_task_cancelled(const std::exception_ptr& failure) noexcept {
    if (failure == nullptr) {
        return false;
    }
    // Rethrow-and-catch is the only portable type test over an exception_ptr, and it cannot
    // throw across this function: the catch ends the stack unwinding.
    try {
        std::rethrow_exception(failure);
    } catch (const TaskCancelled&) {
        return true;
    } catch (...) {
        return false;
    }
}

std::exception_ptr first_failure(const std::vector<std::exception_ptr>& failures) {
    for (const std::exception_ptr& failure : failures) {
        if (failure != nullptr) {
            return failure;
        }
    }
    return nullptr;
}

} // namespace detail

} // namespace tessera::runtime
