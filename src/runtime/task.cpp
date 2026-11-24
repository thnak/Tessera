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
      token_(std::move(context.token)),
      clock_(context.clock),
      timers_(context.timers) {}

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
    // The resume goes on the unbounded continuation lane (issue #843): it is never dropped, so a
    // full fire-and-forget queue cannot hang a task forever. The queued keep-alive reference is
    // what a `std::function` wrapper used to provide, without an allocation per post.
    if (queued_keepalive_ == nullptr) {
        queued_keepalive_ = shared_from_this();
    }
    if (!queue_->post_continuation(priority_, *this)) {
        // Already queued: a second resume is a no-op, exactly like a double `start()`.
        return true;
    }
    return true;
}

void TaskStateBase::arm_cancel_wakeup() {
    if (done_ || cancel_wakeup_.active()) {
        return;
    }
    // The callback runs synchronously on whichever thread cancels the token, so it must only
    // post: `post_resume` is lock-light, never touches the frame, and a resume that is already
    // queued absorbs it. The actual cancellation check happens in the queued resume, on the drain
    // thread. `post_resume` is public, so the lambda can hold the state strongly; that strong
    // reference lives only until the registration is released in `finish()`.
    cancel_wakeup_ = token_.on_cancel([self = shared_from_this()] { self->post_resume(); });
}

void TaskStateBase::resume_continuation() {
    // Drop the queued reference last: `resume_from_queue` may finish the task and resume its
    // continuation, and a re-post made by that resume must see a clean slot.
    const std::shared_ptr<TaskStateBase> keepalive = std::move(queued_keepalive_);
    resume_from_queue();
}

void TaskStateBase::resume_from_queue() {
    if (done_) {
        return;
    }
    if (token_.cancelled()) {
        cancelled_ = true;
        cancellation_ = token_.reason().value_or(CancelReason::Requested);
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
    // Disarm the eager-cancellation wakeup first: a finished task never keeps a callback on its
    // token, and a cancel racing this release is absorbed by the `done_` check in
    // `resume_from_queue`.
    cancel_wakeup_.reset();
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
    // The wakeup registration holds this state through its callback capture; an abandoned task
    // releases it here, together with the continuation, so nothing pins the pair.
    cancel_wakeup_.reset();
}

} // namespace detail

} // namespace tessera::runtime
