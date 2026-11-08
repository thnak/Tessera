// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/async.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace tessera::runtime {

// Thrown at the co_await boundary when the awaited task observed cancellation. Cancellation is
// cooperative: it is checked immediately before a task resumes, never asynchronously.
class TaskCancelled : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override;
};

// Scheduling context for one task. A task coroutine function takes it as its first parameter; the
// promise copies it, so the queue and the token must outlive the task.
struct TaskContext {
    CompletionQueue* queue = nullptr;
    CompletionPriority priority = CompletionPriority::Visible;
    CancellationToken token;
};

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
class TaskStateBase : public std::enable_shared_from_this<TaskStateBase> {
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

protected:
    // Runs in the queued completion: checks cancellation before touching the coroutine frame.
    void resume_from_queue();
    virtual void resume_coroutine() = 0;
    virtual void mark_cancelled() = 0;

    std::exception_ptr exception_;

private:
    bool post_resume();

    CompletionQueue* queue_ = nullptr;
    CompletionPriority priority_ = CompletionPriority::Visible;
    CancellationToken token_;
    std::shared_ptr<TaskStateBase> continuation_;
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
        exception_ = std::make_exception_ptr(TaskCancelled{});
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
        exception_ = std::make_exception_ptr(TaskCancelled{});
    }

private:
    std::coroutine_handle<> handle_;
};

// Stores `child`'s continuation (the calling task) and posts the child's first resume. Any
// failure happens before the caller suspends, so it surfaces as an exception at the co_await.
template<typename ParentPromise>
bool attach_continuation(TaskStateBase& child, std::coroutine_handle<ParentPromise> parent) {
    std::shared_ptr<TaskStateBase> parent_state = parent.promise().self_.lock();
    if (parent_state == nullptr) {
        throw std::logic_error("task awaited outside a Tessera task coroutine");
    }
    if (child.started() || child.done()) {
        throw std::logic_error("task is already started");
    }
    child.set_continuation(std::move(parent_state));
    if (!child.start()) {
        throw std::runtime_error("completion queue rejected the task resume");
    }
    return true;
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
