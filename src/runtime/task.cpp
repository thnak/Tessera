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

void TaskPromiseBase::declare_no_resume() noexcept {
    // The promise holds the state weakly, so a frame that has already been released is a no-op
    // rather than a dangling write.
    if (const std::shared_ptr<TaskStateBase> state = self_.lock()) {
        state->declare_no_resume();
    }
}

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

void TaskStateBase::declare_no_resume() noexcept {
    // This task will never finish, so it will never resume its continuation. That strong reference
    // is one half of the abandoned-frame reference cycle (ADR-069): dropping it turns the pair into
    // a chain rooted at whoever started the awaiting task, which frees it at that owner's next safe
    // point instead of never.
    //
    // Nothing is destroyed here, and that is the point. The awaiting frame is owned by its starter,
    // and its awaiter still owns this state, so this frame stays alive until that owner drops the
    // awaiting frame. `self` guards the cascade if that ever stops holding: releasing the
    // continuation can release the awaiting frame, whose awaiter is the other reference to this
    // state, and this function runs inside the frame that state owns.
    const std::shared_ptr<TaskStateBase> self = weak_from_this().lock();
    continuation_.reset();
}

} // namespace detail

} // namespace tessera::runtime
