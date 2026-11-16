// SPDX-License-Identifier: MIT
//
// Fixtures for the `appfw::TaskScope` tests (AF2, issue #174). Deterministic by construction: every
// child coroutine hops a `ManualExecutor`-style `CompletionQueue` that the test drains by hand, so
// there is no wall clock, no sleep, no thread and no network anywhere in this suite.
//
// The coroutines take `runtime::TaskContext` as their first parameter because `Task`'s promise
// consumes it there (see `docs/design/services/common-contract.md` section 1.1); `scope.spawn(...)`
// starts the task and the scope's token is what cancels it.

#pragma once

#include "tessera/appfw/task_scope.hpp"

#include <coroutine>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace appfw_task_test {

namespace appfw = tessera::appfw;
namespace runtime = tessera::runtime;

inline int failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_task_scope: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Suspends once through the completion queue, so the test — not the coroutine — decides when the
// child resumes. This is the deterministic "await external work" of the real runtime.
struct QueueHop {
    runtime::CompletionQueue* queue = nullptr;
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) const {
        queue->post(runtime::CompletionPriority::Visible, [handle] { handle.resume(); });
    }
    void await_resume() const noexcept {}
};

// Suspends and is never resumed: the only thing that can end such a child is cancellation (which
// stops its *next* resume) or scope destruction.
struct ParkForever {
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

// Drains the queue. Every child resume is a posted completion, so a full drain runs the task graph
// as far as it can go without the test advancing anything else.
inline std::size_t pump(runtime::CompletionQueue& queue, std::size_t budget = 4096) {
    std::size_t executed = 0;
    while (!queue.empty() && executed < budget) {
        executed += queue.run(1);
    }
    return executed;
}

// Runs one completion at a time until `done()` holds. This is how a test stops a child *mid-body*
// without hard-coding how many queue hops a spawn costs: the count is a runtime fact, not a
// constant the test would have to keep in sync with the task machinery.
template<typename Predicate>
std::size_t pump_until(runtime::CompletionQueue& queue, Predicate done, std::size_t budget = 4096) {
    std::size_t executed = 0;
    while (!done() && executed < budget) {
        if (queue.run(1) == 0) {
            break;
        }
        ++executed;
    }
    return executed;
}

// What one child did, recorded by the child itself so a test can prove a cancelled body never ran
// again (rather than merely that the scope stopped counting it).
struct ChildLog {
    int entries = 0;
    int steps = 0;
    int completions = 0;
};

// A child that runs `steps` queue hops and then returns. Each hop is one posted resume, which is
// the point: cancellation is observed at a resume, exactly as the runtime implements it.
inline runtime::Task<void> hop_child(runtime::TaskContext context, runtime::CompletionQueue& queue,
                                     ChildLog& log, int steps) {
    ++log.entries;
    for (int step = 0; step < steps; ++step) {
        co_await QueueHop{&queue};
        // Re-checked after every resume so a cancelled child cannot advance further even if the
        // runtime resumed it for some reason. Cancellation remains cooperative; the scope's token
        // is what makes this loop exit.
        if (context.token.cancelled()) {
            co_return;
        }
        ++log.steps;
    }
    ++log.completions;
    co_return;
}

// A child that never finishes on its own. Only the scope's deadline (or its destruction) ends the
// test's interest in it.
inline runtime::Task<void> parked_child(runtime::TaskContext, ChildLog& log) {
    ++log.entries;
    co_await ParkForever{};
    ++log.completions;
    co_return;
}

// A child that throws, to prove the scope converts a child's failure to a counter instead of
// letting it escape into the frame loop.
inline runtime::Task<void> throwing_child(runtime::TaskContext, ChildLog& log) {
    ++log.entries;
    throw std::runtime_error("child exploded");
    co_return;
}

} // namespace appfw_task_test
