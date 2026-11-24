// SPDX-License-Identifier: MIT
//
// Private machinery for the task combinators (issue #844): the driver coroutines, the race
// states, and the awaiters. Include tessera/runtime/combinators.hpp instead - this file is
// an implementation detail of that public header, split out only to keep both under the
// file-size limit.

#pragma once

#include "tessera/runtime/task.hpp"

#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::runtime {

// What `when_all` throws when at least one child failed. It carries every child failure, so no
// failure is lost, and the first failure in argument order is reachable through `first()`.
//
// The rethrow rule at the `co_await` is deliberately two-part. When the FIRST failure in argument
// order is a `TaskCancelled` - normally the awaiting task's own cancellation flowing to the
// children - the original `TaskCancelled` is rethrown directly, so
// `catch (const TaskCancelled&)` keeps working at the cancellation boundary. For every other
// failure the aggregate itself is thrown, including when a single child failed; code that wants
// the original exception calls `first()` and rethrows it. One rule, no count-dependent type.
// Declared here next to the awaiters that throw it; `combinators.hpp` re-exports it.
class TaskAggregateError : public std::exception {
public:
    TaskAggregateError() = default;
    explicit TaskAggregateError(std::vector<std::exception_ptr> failures);

    [[nodiscard]] const char* what() const noexcept override;
    // Every child failure in argument order; entries are null for children that succeeded.
    [[nodiscard]] const std::vector<std::exception_ptr>& failures() const noexcept {
        return failures_;
    }
    // The first non-null failure in argument order, or a null pointer for a default-constructed
    // error.
    [[nodiscard]] const std::exception_ptr& first() const noexcept;

private:
    std::vector<std::exception_ptr> failures_;
    std::size_t first_index_ = 0;
    std::string message_;
};

} // namespace tessera::runtime

namespace tessera::runtime::detail {

inline constexpr std::size_t kNoWinner = static_cast<std::size_t>(-1);

// The tuple `when_all` resolves to, in argument order. `std::tuple` cannot hold `void`, so a
// `Task<void>` child contributes `std::monostate` instead of skipping the slot.
template<typename... Ts>
using WhenAllResult = std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;

// True when `failure` carries a `TaskCancelled` (the cancellation flow-through case). Defined in
// combinators.cpp; rethrow-and-catch is the only portable type test over an `exception_ptr`.
[[nodiscard]] bool is_task_cancelled(const std::exception_ptr& failure) noexcept;

// The first non-null failure in argument order, or null when every child succeeded.
[[nodiscard]] std::exception_ptr first_failure(const std::vector<std::exception_ptr>& failures);

// Moves one stored child result out of its slot.
template<typename V> V take_slot(std::optional<V>& slot) {
    return std::move(*slot);
}
inline std::monostate take_slot(std::monostate& slot) {
    (void)slot;
    return {};
}

// The task a driver coroutine runs under. Drivers never rethrow: a child's exception is data
// recorded into the race state, which is what lets every sibling finish when one fails.
using DriverTask = Task<void>;

// Runs one child of a `when_all` pack to completion and records the outcome. `Index` addresses
// the state's value slot, so the child's result lands in argument order.
template<typename State, std::size_t Index, typename T>
DriverTask make_all_driver(TaskContext, std::shared_ptr<State> state, Task<T> child) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(child);
        } else {
            state->template store_value<Index>(co_await std::move(child));
        }
    } catch (...) {
        state->child_finished(Index, std::current_exception());
        co_return;
    }
    state->child_finished(Index, nullptr);
}

// The same for a `when_all_range` child, whose slot is addressed at run time.
template<typename State, typename T>
DriverTask make_all_range_driver(TaskContext, std::shared_ptr<State> state, std::size_t index,
                                 Task<T> child) {
    try {
        state->store_value(index, co_await std::move(child));
    } catch (...) {
        state->child_finished(index, std::current_exception());
        co_return;
    }
    state->child_finished(index, nullptr);
}

// Runs one `when_any` child to completion; only the failure (if any) is recorded, because the
// winner's identity is what the caller asked for. Losers keep running to completion.
template<typename State, typename T>
DriverTask make_any_driver(TaskContext, std::shared_ptr<State> state, std::size_t index,
                           Task<T> child) {
    try {
        co_await std::move(child);
    } catch (...) {
        state->child_finished(index, std::current_exception());
        co_return;
    }
    state->child_finished(index, nullptr);
}

// The value-recording variant used by `when_any_value`, which must hand back the winner's result.
template<typename State, typename T>
DriverTask make_any_value_driver(TaskContext, std::shared_ptr<State> state, std::size_t index,
                                 Task<T> child) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(child);
        } else {
            state->store_value(index, co_await std::move(child));
        }
    } catch (...) {
        state->child_finished(index, std::current_exception());
        co_return;
    }
    state->child_finished(index, nullptr);
}

// Shared scheduling core of one race: the child context, and the awaiting parent's state so the
// gate can post the parent's single resume. All mutation happens on the single drain thread
// (drivers and gate share one queue/priority), so plain members suffice.
template<typename Derived> class CombinatorState : public std::enable_shared_from_this<Derived> {
public:
    virtual ~CombinatorState() = default;
    CombinatorState(const CombinatorState&) = delete;
    CombinatorState& operator=(const CombinatorState&) = delete;

    // Records child `index`'s failure (null on success) and applies the gate rule. Runs inside a
    // driver, on the drain thread only.
    virtual void child_finished(std::size_t index, std::exception_ptr failure) = 0;
    // Starts every child through its driver. Called once, from the awaiter's `await_suspend`.
    virtual void start_children() = 0;

    void set_parent(std::shared_ptr<TaskStateBase> parent) {
        parent_ = std::move(parent);
    }
    [[nodiscard]] TaskContext context() const {
        return context_;
    }

protected:
    explicit CombinatorState(std::size_t child_count, TaskContext context)
        : child_count_(child_count),
          context_(std::move(context)) {}

    // Schedules the awaiting parent's resume; the resume coalesces if one is already queued, and
    // a parent resumed by an earlier gate event absorbs later posts through its `done_` check.
    void wake_parent() {
        if (parent_ != nullptr) {
            parent_->post_resume();
            parent_.reset();
        }
    }

    std::size_t child_count_ = 0;
    TaskContext context_;
    std::shared_ptr<TaskStateBase> parent_;
};

// The per-child failures of one `when_all`, in argument order.
struct WhenAllOutcome {
    std::vector<std::exception_ptr> failures;
};

// `when_all` over a parameter pack: values land in per-index slots, failures in argument order,
// and the gate counts every child down before waking the parent.
template<typename... Ts>
class WhenAllStateTyped final : public CombinatorState<WhenAllStateTyped<Ts...>> {
public:
    WhenAllStateTyped(TaskContext context, Task<Ts>... children)
        : CombinatorState<WhenAllStateTyped>(sizeof...(Ts), std::move(context)),
          children_(std::move(children)...) {
        this->outcome_.failures.assign(sizeof...(Ts), nullptr);
    }

    void child_finished(std::size_t index, std::exception_ptr failure) override {
        if (done_) {
            return;
        }
        outcome_.failures[index] = std::move(failure);
        if (--remaining_ == 0) {
            done_ = true;
            this->wake_parent();
        }
    }

    void start_children() override {
        start_impl(std::index_sequence_for<Ts...>{});
    }

    template<std::size_t Index, typename V> void store_value(V value) {
        std::get<Index>(values_).emplace(std::move(value));
    }

    [[nodiscard]] const WhenAllOutcome& outcome() const noexcept {
        return outcome_;
    }

    [[nodiscard]] WhenAllResult<Ts...> take_values() {
        return std::apply([](auto&... slots) { return WhenAllResult<Ts...>(take_slot(slots)...); },
                          values_);
    }

private:
    template<std::size_t... Index> void start_impl(std::index_sequence<Index...>) {
        // Left-to-right fold over the launches, so each child is moved out of the tuple exactly
        // once. The driver task is started and released at once: the queue's keep-alive holds the
        // driver frame until its first resume, and the child's continuation holds it from there.
        // The driver context carries NO token on purpose: a driver must never be cancelled
        // (a cancelled driver's body never runs, so it could never record the outcome and the
        // gate would hang); the children carry their own contexts and tokens.
        const TaskContext driver_ctx{this->context_.queue, this->context_.priority, {}};
        (void)std::initializer_list<int>{(
            [&] {
                Task<void> driver = make_all_driver<WhenAllStateTyped, Index, Ts>(
                    driver_ctx, this->shared_from_this(), std::move(std::get<Index>(children_)));
                return driver.start() ? 0 : 0;
            }(),
            0)...};
    }

    WhenAllOutcome outcome_;
    std::size_t remaining_ = sizeof...(Ts);
    bool done_ = false;
    std::tuple<Task<Ts>...> children_;
    std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, std::optional<Ts>>...>
        values_;
};

// `when_all` over a range of same-typed children (non-void results; a range of `Task<void>` has
// nothing to collect, so `when_all_range` rejects it).
template<typename T> class WhenAllRangeState final : public CombinatorState<WhenAllRangeState<T>> {
public:
    WhenAllRangeState(std::size_t child_count, TaskContext context)
        : CombinatorState<WhenAllRangeState>(child_count, std::move(context)) {
        this->outcome_.failures.assign(child_count, nullptr);
        values_.resize(child_count);
        remaining_ = child_count;
    }

    void child_finished(std::size_t index, std::exception_ptr failure) override {
        if (done_) {
            return;
        }
        outcome_.failures[index] = std::move(failure);
        if (--remaining_ == 0) {
            done_ = true;
            this->wake_parent();
        }
    }

    void start_children() override {
        // Token-less driver context: see `WhenAllStateTyped::start_impl` for why a driver must
        // never carry a cancellable token.
        const TaskContext driver_ctx{this->context_.queue, this->context_.priority, {}};
        for (std::size_t index = 0; index < this->child_count_; ++index) {
            Task<void> driver = make_all_range_driver<WhenAllRangeState>(
                driver_ctx, this->shared_from_this(), index, std::move(children_[index]));
            (void)driver.start();
        }
        children_.clear();
    }

    void store_value(std::size_t index, T value) {
        values_[index].emplace(std::move(value));
    }

    [[nodiscard]] const WhenAllOutcome& outcome() const noexcept {
        return outcome_;
    }

    [[nodiscard]] std::vector<T> take_values() {
        std::vector<T> values;
        values.reserve(values_.size());
        for (auto& slot : values_) {
            values.push_back(std::move(*slot));
        }
        return values;
    }

    void take_children(std::span<Task<T>> children) {
        children_.assign(std::make_move_iterator(children.begin()),
                         std::make_move_iterator(children.end()));
    }

private:
    WhenAllOutcome outcome_;
    std::size_t remaining_ = 0;
    bool done_ = false;
    std::vector<std::optional<T>> values_;
    std::vector<Task<T>> children_;
};

// Gate rule shared by both `when_any` shapes: the first finisher by completion order wins, and
// later finishers are no-ops (their drivers keep running to completion).
template<typename Derived> class WhenAnyStateBase : public CombinatorState<Derived> {
public:
    void child_finished(std::size_t index, std::exception_ptr failure) override {
        if (winner_ != kNoWinner) {
            return;
        }
        winner_ = index;
        failure_ = std::move(failure);
        this->wake_parent();
    }

    [[nodiscard]] std::size_t winner() const noexcept {
        return winner_;
    }
    [[nodiscard]] const std::exception_ptr& failure() const noexcept {
        return failure_;
    }

protected:
    using CombinatorState<Derived>::CombinatorState;

    std::size_t winner_ = kNoWinner;
    std::exception_ptr failure_;
};

// `when_any` over a parameter pack; only indices, no values.
template<typename... Ts>
class WhenAnyPackState final : public WhenAnyStateBase<WhenAnyPackState<Ts...>> {
public:
    WhenAnyPackState(TaskContext context, Task<Ts>... children)
        : WhenAnyStateBase<WhenAnyPackState>(sizeof...(Ts), std::move(context)),
          children_(std::move(children)...) {}

    void start_children() override {
        start_impl(std::index_sequence_for<Ts...>{});
    }

private:
    template<std::size_t... Index> void start_impl(std::index_sequence<Index...>) {
        // Token-less driver context: see `WhenAllStateTyped::start_impl` for why a driver must
        // never carry a cancellable token.
        const TaskContext driver_ctx{this->context_.queue, this->context_.priority, {}};
        (void)std::initializer_list<int>{(
            [&] {
                Task<void> driver =
                    make_any_driver<WhenAnyPackState>(driver_ctx, this->shared_from_this(), Index,
                                                      std::move(std::get<Index>(children_)));
                return driver.start() ? 0 : 0;
            }(),
            0)...};
    }

    std::tuple<Task<Ts>...> children_;
};

// `when_any` over a range (and the single-type `when_any_value`), which also stores each child's
// result so the winner's value survives the race.
template<typename T> class WhenAnyRangeState final : public WhenAnyStateBase<WhenAnyRangeState<T>> {
public:
    WhenAnyRangeState(std::size_t child_count, TaskContext context)
        : WhenAnyStateBase<WhenAnyRangeState>(child_count, std::move(context)),
          values_(child_count) {}

    void start_children() override {
        // Token-less driver context: see `WhenAllStateTyped::start_impl` for why a driver must
        // never carry a cancellable token.
        const TaskContext driver_ctx{this->context_.queue, this->context_.priority, {}};
        for (std::size_t index = 0; index < this->child_count_; ++index) {
            Task<void> driver = make_any_value_driver<WhenAnyRangeState>(
                driver_ctx, this->shared_from_this(), index, std::move(children_[index]));
            (void)driver.start();
        }
        children_.clear();
    }

    template<typename U = T>
        requires(!std::is_void_v<U>)
    void store_value(std::size_t index, U value) {
        values_[index].emplace(std::move(value));
    }

    [[nodiscard]] T take_value(std::size_t index) {
        if constexpr (std::is_void_v<T>) {
            (void)index;
            return;
        } else {
            return std::move(*values_[index]);
        }
    }

    void take_children(std::span<Task<T>> children) {
        children_.assign(std::make_move_iterator(children.begin()),
                         std::make_move_iterator(children.end()));
    }

private:
    std::vector<std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>> values_;
    std::vector<Task<T>> children_;
};

// The awaiter body shared by every combinator: capture the parent state through the coroutine
// handle's promise (the same path `attach_continuation` uses), arm its eager-cancellation hook,
// and start the drivers. The parent's own continuation slot is never used, so exactly one gate
// event wakes it.
template<typename StateT> class CombinatorAwaiter {
public:
    explicit CombinatorAwaiter(std::shared_ptr<StateT> state)
        : state_(std::move(state)) {}

    CombinatorAwaiter(CombinatorAwaiter&&) = default;
    CombinatorAwaiter(const CombinatorAwaiter&) = delete;
    CombinatorAwaiter& operator=(const CombinatorAwaiter&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return state_ == nullptr;
    }

    template<typename ParentPromise>
    bool await_suspend(std::coroutine_handle<ParentPromise> parent) {
        std::shared_ptr<TaskStateBase> parent_state = parent.promise().self_.lock();
        if (parent_state == nullptr) {
            throw std::logic_error("task awaited outside a Tessera task coroutine");
        }
        parent_state->arm_cancel_wakeup();
        state_->set_parent(std::move(parent_state));
        state_->start_children();
        return true;
    }

protected:
    ~CombinatorAwaiter() = default;

    std::shared_ptr<StateT> state_;
};

template<typename... Ts>
class WhenAllPackAwaiter final : public CombinatorAwaiter<WhenAllStateTyped<Ts...>> {
public:
    using CombinatorAwaiter<WhenAllStateTyped<Ts...>>::CombinatorAwaiter;

    WhenAllResult<Ts...> await_resume() {
        // Cancellation flow-through first: rethrow the original `TaskCancelled` so
        // `catch (const TaskCancelled&)` keeps working at the await boundary. Everything else
        // travels as the aggregate, which keeps every failure visible.
        if (const std::exception_ptr failure = first_failure(this->state_->outcome().failures);
            failure != nullptr) {
            if (is_task_cancelled(failure)) {
                std::rethrow_exception(failure);
            }
            throw TaskAggregateError(this->state_->outcome().failures);
        }
        return this->state_->take_values();
    }
};

template<typename T>
class WhenAllRangeAwaiter final : public CombinatorAwaiter<WhenAllRangeState<T>> {
public:
    using CombinatorAwaiter<WhenAllRangeState<T>>::CombinatorAwaiter;

    std::vector<T> await_resume() {
        // Same rule as the pack form: the first cancellation in span order is rethrown as-is,
        // everything else as the aggregate; reaching the move-out means every child succeeded
        // (an empty range lands here too, without suspending).
        if (const std::exception_ptr failure = first_failure(this->state_->outcome().failures);
            failure != nullptr) {
            if (is_task_cancelled(failure)) {
                std::rethrow_exception(failure);
            }
            throw TaskAggregateError(this->state_->outcome().failures);
        }
        return this->state_->take_values();
    }
};

// Resolves to the winner's index, or rethrows the winner's exception. The issue's "the caller
// inspects the winner" is realizable only through the value form (`when_any_value`): the child
// tasks are owned by the race, so silently returning an index for a failed winner would drop its
// exception.
template<typename StateT> class WhenAnyAwaiter final : public CombinatorAwaiter<StateT> {
public:
    using CombinatorAwaiter<StateT>::CombinatorAwaiter;

    std::size_t await_resume() {
        if (this->state_->winner() == kNoWinner) {
            throw std::logic_error("when_any resumed without a finished child");
        }
        if (this->state_->failure() != nullptr) {
            std::rethrow_exception(this->state_->failure());
        }
        return this->state_->winner();
    }
};

template<typename T>
class WhenAnyValueAwaiter final : public CombinatorAwaiter<WhenAnyRangeState<T>> {
public:
    using CombinatorAwaiter<WhenAnyRangeState<T>>::CombinatorAwaiter;

    T await_resume() {
        if (this->state_->winner() == kNoWinner) {
            throw std::logic_error("when_any resumed without a finished child");
        }
        if (this->state_->failure() != nullptr) {
            std::rethrow_exception(this->state_->failure());
        }
        return this->state_->take_value(this->state_->winner());
    }
};

} // namespace tessera::runtime::detail
