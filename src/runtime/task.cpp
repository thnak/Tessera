// SPDX-License-Identifier: MIT

#include "tessera/runtime/task.hpp"

#include <utility>

namespace tessera::runtime {

const char* TaskCancelled::what() const noexcept {
    return "task cancelled";
}

namespace detail {

TaskStateBase::TaskStateBase(TaskContext context)
    : queue_(context.queue),
      priority_(context.priority),
      token_(std::move(context.token)) {}

bool TaskStateBase::start() {
    if (started_ || done_) {
        return false;
    }
    if (!post_resume()) {
        return false;
    }
    started_ = true;
    return true;
}

bool TaskStateBase::post_resume() {
    if (queue_ == nullptr) {
        return false;
    }
    // The queue token is intentionally empty: cancellation is checked in resume_from_queue, where
    // the task can finish (and notify its awaiter) instead of being dropped silently.
    return queue_->post(priority_, [self = shared_from_this()] { self->resume_from_queue(); });
}

void TaskStateBase::resume_from_queue() {
    if (done_) {
        return;
    }
    if (token_.cancelled()) {
        cancelled_ = true;
        mark_cancelled();
        finish();
        return;
    }
    resume_coroutine();
}

void TaskStateBase::set_continuation(std::shared_ptr<TaskStateBase> continuation) {
    continuation_ = std::move(continuation);
}

void TaskStateBase::finish() {
    if (done_) {
        return;
    }
    done_ = true;
    std::shared_ptr<TaskStateBase> continuation = std::move(continuation_);
    if (continuation != nullptr) {
        continuation->post_resume();
    }
}

void TaskStateBase::set_exception(std::exception_ptr exception) {
    exception_ = std::move(exception);
}

} // namespace detail

} // namespace tessera::runtime
