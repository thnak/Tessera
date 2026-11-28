// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task_context.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace tessera::runtime {

template<typename T> class Task;

namespace detail {

class TaskStateBase;

template<typename T> class TaskState;

// Promise base shared by Task<T> and Task<void>. It keeps the scheduling context and a weak
// reference to the task state; an awaited child uses that reference to keep its parent frame
// alive while the parent is suspended.
class TaskPromiseBase {
public:
    template<typename... Rest>
    explicit TaskPromiseBase(TaskContext context, Rest&...)
        : context_(std::move(context)) {}

    TaskPromiseBase(const TaskPromiseBase&) = delete;
    TaskPromiseBase& operator=(const TaskPromiseBase&) = delete;

    // Declares that this coroutine will never be resumed: the awaitable it is suspended on holds no
    // handle to the frame and nothing will post a resume for it. The runtime then drops the strong
    // reference to the awaiting task, which is what turns an abandoned pair from a reference cycle
    // nothing can break into a chain its owner frees normally (ADR-069). An awaitable that can
    // resume the frame later - a queued completion, a callback, a timer - must not call this,
    // because the awaiting frame would be freed while a resume is still coming.
    void declare_no_resume() noexcept;

    std::weak_ptr<TaskStateBase> self_;

protected:
    TaskContext context_;
};

// Final awaiter: marks the task done and posts the awaiting task's resume through its own queue,
// so a continuation never runs inline on the completing stack.
template<typename T> class TaskFinalAwaiter {
public:
    explicit TaskFinalAwaiter(TaskState<T>* state)
        : state_(state) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<>) const noexcept {
        state_->finish();
    }
    void await_resume() const noexcept {}

private:
    TaskState<T>* state_ = nullptr;
};

// Shared ownership and lifecycle for one coroutine frame. The frame is destroyed exactly once,
// when the last reference to the state drops; no thread is created, joined, or detached here.
//
// It is also the intrusive node a queued resume links through (`detail::ContinuationNode`), which
// is what keeps a resume unbounded and allocation-free (issue #843): the queue stores a pointer to
// this state, the state stores an extra shared reference while queued, and nothing is dropped.
class TaskStateBase : public detail::ContinuationNode,
                      public std::enable_shared_from_this<TaskStateBase> {
public:
    explicit TaskStateBase(TaskContext context);
    virtual ~TaskStateBase() = default;

    TaskStateBase(const TaskStateBase&) = delete;
    TaskStateBase& operator=(const TaskStateBase&) = delete;

    // Schedules the first resume through the task's queue. Returns false when the task was
    // already started/finished or when the queue rejected the post (full or missing).
    bool start();

    // Registers the awaiting task. When this task finishes, the continuation is resumed through
    // its own queue and priority (same queue when the context is propagated).
    void set_continuation(std::shared_ptr<TaskStateBase> continuation);
    void finish();
    void set_exception(std::exception_ptr exception);

    // Posts this task's resume onto its own continuation lane (see `post_resume` below): one call
    // schedules exactly one queued resume, and later calls while that resume is still queued are
    // absorbed by it. `finish()` and the eager-cancellation hook use it to wake a suspended task.
    bool post_resume();

    // Arms the eager-cancellation wakeup: registers one `on_cancel` callback on this task's token
    // that posts the task's resume, so a frame suspended on a child (or on a timer) is cancelled
    // promptly instead of at the child's next completion. The registration is stored here and
    // released in `finish()` and the destructor. Callbacks run on the cancelling thread, so the
    // callback only posts - it never touches the frame or a timer. Idempotent: a second call while
    // armed is a no-op, and a token cancelled before the call runs the post immediately.
    void arm_cancel_wakeup();

    // Records that this task is suspended on an awaitable that will never resume it, so it will
    // never finish and never resume its continuation. The strong reference to that continuation is
    // one half of the abandoned-frame reference cycle, so it is dropped here (ADR-069). Nothing is
    // destroyed: the awaiting frame is still owned by whoever started it, and its awaiter still
    // owns this state, so the pair is freed when that owner drops the awaiting frame.
    void declare_no_resume() noexcept;

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }
    [[nodiscard]] bool done() const noexcept {
        return done_;
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_;
    }
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }
    // The time seams from the task's context, for awaiters that schedule timers (`delay`,
    // `timeout`). Null when the context carried none.
    [[nodiscard]] Clock* clock() const noexcept {
        return clock_;
    }
    [[nodiscard]] TimerQueue* timers() const noexcept {
        return timers_;
    }

protected:
    // Runs in the queued completion: checks cancellation before touching the coroutine frame.
    void resume_from_queue();
    virtual void resume_coroutine() = 0;
    virtual void mark_cancelled() = 0;

    // `detail::ContinuationNode`: the queued-resume entry point (issue #843).
    void resume_continuation() final;
    std::shared_ptr<void> take_queued_reference() noexcept final {
        return std::move(queued_keepalive_);
    }

    std::exception_ptr exception_;

private:
    CompletionQueue* queue_ = nullptr;
    CompletionPriority priority_ = CompletionPriority::Visible;
    CancellationToken token_;
    std::shared_ptr<TaskStateBase> continuation_;
    // Keeps the frame alive while its resume sits on the queue without a `std::function` wrapper.
    // Released inside `resume_continuation`, which runs from the queue.
    std::shared_ptr<TaskStateBase> queued_keepalive_;
    // The armed eager-cancellation wakeup, released in `finish()` (and the destructor) so a
    // finished task never holds a callback on its token.
    CancellationRegistration cancel_wakeup_;

protected:
    // Why the token was cancelled, captured when this task observes it, so `TaskCancelled` carries
    // the reason (issue #845). Read by the derived `mark_cancelled`.
    CancelReason cancellation_ = CancelReason::Requested;

private:
    Clock* clock_ = nullptr;
    TimerQueue* timers_ = nullptr;
    bool started_ = false;
    bool done_ = false;
    bool cancelled_ = false;
};

template<typename T> class TaskState final : public TaskStateBase {
public:
    TaskState(TaskContext context, std::coroutine_handle<> handle)
        : TaskStateBase(std::move(context)),
          handle_(handle) {}

    ~TaskState() override {
        if (handle_ != nullptr) {
            handle_.destroy();
        }
    }

    void set_value(T value) {
        value_.emplace(std::move(value));
    }
    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }
    [[nodiscard]] T take_value() {
        return std::move(*value_);
    }

protected:
    void resume_coroutine() override {
        handle_.resume();
    }
    void mark_cancelled() override {
        exception_ = std::make_exception_ptr(TaskCancelled{cancellation_});
    }

private:
    std::coroutine_handle<> handle_;
    std::optional<T> value_;
};

template<> class TaskState<void> final : public TaskStateBase {
public:
    TaskState(TaskContext context, std::coroutine_handle<> handle)
        : TaskStateBase(std::move(context)),
          handle_(handle) {}

    ~TaskState() override {
        if (handle_ != nullptr) {
            handle_.destroy();
        }
    }

protected:
    void resume_coroutine() override {
        handle_.resume();
    }
    void mark_cancelled() override {
        exception_ = std::make_exception_ptr(TaskCancelled{cancellation_});
    }

private:
    std::coroutine_handle<> handle_;
};

// Stores `child`'s continuation (the calling task) and posts the child's first resume. Any
// failure happens before the caller suspends, so it surfaces as an exception at the co_await.
//
// Eager cancellation (issue #844): after the child is started, the awaiting parent arms its
// wakeup hook, so if the parent's own token is cancelled while it waits, the parent is posted a
// resume immediately; the queued resume observes the cancellation and finishes the parent with
// `TaskCancelled`. A child does not stop - cancelling the parent is not fail-fast - and a task
// that has not started yet keeps the plain start-time check.
template<typename ParentPromise>
bool attach_continuation(TaskStateBase& child, std::coroutine_handle<ParentPromise> parent) {
    std::shared_ptr<TaskStateBase> parent_state = parent.promise().self_.lock();
    if (parent_state == nullptr) {
        throw std::logic_error("task awaited outside a Tessera task coroutine");
    }
    if (child.started() || child.done()) {
        throw std::logic_error("task is already started");
    }
    child.set_continuation(parent_state);
    if (!child.start()) {
        throw std::runtime_error("completion queue rejected the task resume");
    }
    parent_state->arm_cancel_wakeup();
    return true;
}

// Opt-in for an awaitable that provably cannot resume the frame it is suspending: it holds no
// `coroutine_handle`, posts nothing, and registers no callback. Call it from `await_suspend` with
// the handle that was passed in, and only when that is true - an awaitable that can resume later
// must not, because the awaiting frame would be freed while a resume is still coming (ADR-069).
//
// The effect is that the task the frame is awaiting drops its strong reference back to the frame's
// state. Without it an abandoned pair is a reference cycle that only the awaited task finishing can
// break, and a task parked on such an awaitable never finishes.
template<typename Promise> void declare_no_resume(std::coroutine_handle<Promise> handle) noexcept {
    handle.promise().declare_no_resume();
}

} // namespace detail

template<typename T> class TaskAwaiter {
public:
    explicit TaskAwaiter(std::shared_ptr<detail::TaskState<T>> state)
        : state_(std::move(state)) {}

    // A task can be awaited only once; a consumed task completes immediately and throws.
    [[nodiscard]] bool await_ready() const noexcept {
        return state_ == nullptr;
    }

    template<typename ParentPromise>
    bool await_suspend(std::coroutine_handle<ParentPromise> parent) {
        return detail::attach_continuation(*state_, parent);
    }

    T await_resume() {
        if (state_ == nullptr) {
            throw std::logic_error("task was already awaited");
        }
        if (state_->exception() != nullptr) {
            std::rethrow_exception(state_->exception());
        }
        if (!state_->has_value()) {
            throw std::logic_error("task finished without a value");
        }
        return state_->take_value();
    }

private:
    std::shared_ptr<detail::TaskState<T>> state_;
};

template<> class TaskAwaiter<void> {
public:
    explicit TaskAwaiter(std::shared_ptr<detail::TaskState<void>> state)
        : state_(std::move(state)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return state_ == nullptr;
    }

    template<typename ParentPromise>
    bool await_suspend(std::coroutine_handle<ParentPromise> parent) {
        return detail::attach_continuation(*state_, parent);
    }

    void await_resume() {
        if (state_ == nullptr) {
            throw std::logic_error("task was already awaited");
        }
        if (state_->exception() != nullptr) {
            std::rethrow_exception(state_->exception());
        }
    }

private:
    std::shared_ptr<detail::TaskState<void>> state_;
};

// Lazy single-consumer coroutine result. Creating a Task allocates the frame and runs nothing;
// start() (or the first co_await) posts the first resume to the TaskContext queue. Every resume
// runs on a thread that drains that queue, and cancellation is checked before each one.
template<typename T> class Task {
    static_assert(std::is_move_constructible_v<T>, "Task<T> requires a move-constructible result");

public:
    struct promise_type : detail::TaskPromiseBase {
        template<typename... Rest>
        explicit promise_type(TaskContext context, Rest&... rest)
            : detail::TaskPromiseBase(std::move(context), rest...) {}

        Task get_return_object() {
            auto state = std::make_shared<detail::TaskState<T>>(
                this->context_, std::coroutine_handle<promise_type>::from_promise(*this));
            this->self_ = state;
            state_ = state.get();
            return Task(std::move(state));
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }
        detail::TaskFinalAwaiter<T> final_suspend() noexcept {
            return detail::TaskFinalAwaiter<T>(state_);
        }
        void return_value(T value) {
            state_->set_value(std::move(value));
        }
        void unhandled_exception() {
            state_->set_exception(std::current_exception());
        }

    private:
        detail::TaskState<T>* state_ = nullptr;
    };

    Task() = default;
    explicit Task(std::shared_ptr<detail::TaskState<T>> state)
        : state_(std::move(state)) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    // Schedules the first resume through the context queue at the context priority. Lazy tasks
    // run nothing until this is called (or they are awaited). Returns false when the task was
    // already started/finished or the queue rejected the post.
    [[nodiscard]] bool start() {
        return state_ != nullptr && state_->start();
    }

    TaskAwaiter<T> operator co_await() & {
        return TaskAwaiter<T>(std::move(state_));
    }
    TaskAwaiter<T> operator co_await() && {
        return TaskAwaiter<T>(std::move(state_));
    }

    [[nodiscard]] bool started() const noexcept {
        return state_ != nullptr && state_->started();
    }
    [[nodiscard]] bool done() const noexcept {
        return state_ != nullptr && state_->done();
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return state_ != nullptr && state_->cancelled();
    }
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return state_ != nullptr ? state_->exception() : nullptr;
    }

private:
    std::shared_ptr<detail::TaskState<T>> state_;
};

template<> class Task<void> {
public:
    struct promise_type : detail::TaskPromiseBase {
        template<typename... Rest>
        explicit promise_type(TaskContext context, Rest&... rest)
            : detail::TaskPromiseBase(std::move(context), rest...) {}

        Task get_return_object() {
            auto state = std::make_shared<detail::TaskState<void>>(
                this->context_, std::coroutine_handle<promise_type>::from_promise(*this));
            this->self_ = state;
            state_ = state.get();
            return Task(std::move(state));
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }
        detail::TaskFinalAwaiter<void> final_suspend() noexcept {
            return detail::TaskFinalAwaiter<void>(state_);
        }
        void return_void() noexcept {}
        void unhandled_exception() {
            state_->set_exception(std::current_exception());
        }

    private:
        detail::TaskState<void>* state_ = nullptr;
    };

    Task() = default;
    explicit Task(std::shared_ptr<detail::TaskState<void>> state)
        : state_(std::move(state)) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    [[nodiscard]] bool start() {
        return state_ != nullptr && state_->start();
    }

    TaskAwaiter<void> operator co_await() & {
        return TaskAwaiter<void>(std::move(state_));
    }
    TaskAwaiter<void> operator co_await() && {
        return TaskAwaiter<void>(std::move(state_));
    }

    [[nodiscard]] bool started() const noexcept {
        return state_ != nullptr && state_->started();
    }
    [[nodiscard]] bool done() const noexcept {
        return state_ != nullptr && state_->done();
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return state_ != nullptr && state_->cancelled();
    }
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return state_ != nullptr ? state_->exception() : nullptr;
    }

private:
    std::shared_ptr<detail::TaskState<void>> state_;
};

} // namespace tessera::runtime
