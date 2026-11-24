// SPDX-License-Identifier: MIT

// The awaiters of the task combinators (issue #844): the shared `await_suspend` that captures
// the parent state through the coroutine handle's promise, arms its eager-cancellation hook and
// starts the drivers, plus one awaiter per combinator shape. Split from combinator_states.hpp so
// each file stays under the source-size limit; `combinators.hpp` includes both.

#pragma once

#include "tessera/runtime/combinator_states.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

// The states and the awaiters live in the detail namespace beside the task states they drive.
namespace tessera::runtime::detail {

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
