// SPDX-License-Identifier: MIT
//
// Task combinators (issue #844): `when_all` / `when_any` over eager cancellation. A failing child
// never cancels its siblings - every child runs to completion - and the aggregate keeps every
// failure visible. The awaiting parent is woken by the driver tasks, so it never registers itself
// as the continuation of more than one child (a queued resume coalesces through one task state,
// which is single-slot).
//
// Lifetime shape: the shared race state is held by the awaiter and by every driver frame; each
// driver frame is kept alive across its whole execution by the runtime's own chain (queue
// keep-alive while a resume is queued, the child's continuation while the driver is suspended on
// the child), so the state never has to store the driver tasks. Nothing forms a reference cycle,
// and abandoned `when_any` losers keep running to completion under their drivers.

#pragma once

#include "tessera/runtime/combinator_states.hpp"
#include "tessera/runtime/task.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace tessera::runtime {

// `TaskAggregateError` is declared in `combinator_states.hpp`, next to the awaiters that throw it;
// this header re-exports it as part of the public combinator surface. See that declaration for the
// rethrow rule (cancellation flow-through vs the aggregate).

// The tuple `when_all` resolves to, in argument order - defined in `combinator_states.hpp` and
// re-exported here, because it is part of the public result type.
template<typename... Ts> using WhenAllResult = detail::WhenAllResult<Ts...>;

namespace detail {

// Rethrows `captured` after the co_await. A rethrow from a custom awaiter's `await_resume` that is
// the last thing a coroutine body does trips an MSVC codegen edge: the exception never reaches the
// promise's `unhandled_exception` and silently vanishes at the frame epilogue. Catching in-frame
// and rethrowing from a trailing statement keeps the unwind path well-formed.
template<typename T> [[noreturn]] void rethrow_captured(std::exception_ptr captured) {
    std::rethrow_exception(std::move(captured));
}

} // namespace detail

// Awaits every child concurrently; children run to completion even when some fail. Resolves to
// the values as a tuple in argument order (`Task<void>` children contribute `std::monostate`), or
// throws per the `TaskAggregateError` rule. Every child is started here, and a `Task` can be
// awaited only once, so each child must be a distinct frame.
template<typename... Ts>
[[nodiscard]] Task<WhenAllResult<Ts...>> when_all(TaskContext ctx, Task<Ts>... children) {
    static_assert(sizeof...(Ts) > 0, "when_all needs at least one child task");
    static_assert((std::conjunction_v<std::is_move_constructible<
                       std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>>...>),
                  "when_all child results must be move-constructible");
    auto state =
        std::make_shared<detail::WhenAllStateTyped<Ts...>>(std::move(ctx), std::move(children)...);
    std::exception_ptr captured;
    try {
        co_return co_await detail::WhenAllPackAwaiter<Ts...>(std::move(state));
    } catch (...) {
        captured = std::current_exception();
    }
    detail::rethrow_captured<WhenAllResult<Ts...>>(std::move(captured));
}

// The range form of `when_all`: every child of `children` is started (the span must be mutable -
// the children are moved out of it) and the values resolve as a vector in span order. An empty
// span resolves to an empty vector without suspending anything.
template<typename T>
[[nodiscard]] Task<std::vector<T>> when_all_range(TaskContext ctx, std::span<Task<T>> children) {
    static_assert(!std::is_void_v<T>,
                  "when_all_range collects values; await each Task<void> child directly instead");
    if (children.empty()) {
        co_return {};
    }
    auto state = std::make_shared<detail::WhenAllRangeState<T>>(children.size(), std::move(ctx));
    state->take_children(children);
    std::exception_ptr captured;
    try {
        co_return co_await detail::WhenAllRangeAwaiter<T>(std::move(state));
    } catch (...) {
        captured = std::current_exception();
    }
    detail::rethrow_captured<std::vector<T>>(std::move(captured));
}

// Resolves to the index of the FIRST child to finish, by completion order. The other children are
// NOT cancelled: they run to completion under their drivers. A winner that finished with an
// exception or through cancellation counts as finished; its exception is rethrown here, because
// the race owns the child tasks - use `when_any_value` to get the winner's value instead.
template<typename... Ts>
[[nodiscard]] Task<std::size_t> when_any(TaskContext ctx, Task<Ts>... children) {
    static_assert(sizeof...(Ts) > 0, "when_any needs at least one child task");
    auto state =
        std::make_shared<detail::WhenAnyPackState<Ts...>>(std::move(ctx), std::move(children)...);
    std::exception_ptr captured;
    try {
        co_return co_await detail::WhenAnyAwaiter<detail::WhenAnyPackState<Ts...>>(
            std::move(state));
    } catch (...) {
        captured = std::current_exception();
    }
    detail::rethrow_captured<std::size_t>(std::move(captured));
}

// The range form of `when_any`: the index of the first child of `children` to finish. Throws
// `std::invalid_argument` for an empty span; the throw surfaces at the `co_await` like every
// combinator input error, because a coroutine body only runs on its first resume.
template<typename T>
[[nodiscard]] Task<std::size_t> when_any_range(TaskContext ctx, std::span<Task<T>> children) {
    if (children.empty()) {
        throw std::invalid_argument("when_any_range needs at least one child task");
    }
    auto state = std::make_shared<detail::WhenAnyRangeState<T>>(children.size(), std::move(ctx));
    state->take_children(children);
    std::exception_ptr captured;
    try {
        co_return co_await detail::WhenAnyAwaiter<detail::WhenAnyRangeState<T>>(std::move(state));
    } catch (...) {
        captured = std::current_exception();
    }
    detail::rethrow_captured<std::size_t>(std::move(captured));
}

// Convenience form for same-typed children: the winner's value, or the winner's exception
// rethrown. It is the value-returning counterpart of `when_any` for the common case where every
// child produces a `T`.
template<typename... Ts>
    requires(std::conjunction_v<std::is_same<Ts, std::tuple_element_t<0, std::tuple<Ts...>>>...>)
[[nodiscard]] Task<std::tuple_element_t<0, std::tuple<Ts...>>>
when_any_value(TaskContext ctx, Task<Ts>... children) {
    static_assert(sizeof...(Ts) > 0, "when_any_value needs at least one child task");
    using T = std::tuple_element_t<0, std::tuple<Ts...>>;
    auto state = std::make_shared<detail::WhenAnyRangeState<T>>(sizeof...(Ts), std::move(ctx));
    std::array<Task<T>, sizeof...(Ts)> typed_children{std::move(children)...};
    state->take_children(std::span<Task<T>>(typed_children));
    std::exception_ptr captured;
    try {
        co_return co_await detail::WhenAnyValueAwaiter<T>(std::move(state));
    } catch (...) {
        captured = std::current_exception();
    }
    detail::rethrow_captured<T>(std::move(captured));
}

} // namespace tessera::runtime
