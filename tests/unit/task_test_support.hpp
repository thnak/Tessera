// SPDX-License-Identifier: MIT
//
// Shared fixtures for the task tests, which are split across `task_tests.cpp` (the base Task
// contract) and `task_cancellation_tests.cpp` (#843 continuations, #845 cancellation, #842 wake).
// Both are separate executables, registered in tests/unit/CMakeLists.txt.

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"

#include <cstddef>
#include <iostream>

namespace tessera::task_test {

using tessera::runtime::CancellationSource;
using tessera::runtime::CompletionPriority;
using tessera::runtime::CompletionQueue;
using tessera::runtime::Task;
using tessera::runtime::TaskCancelled;
using tessera::runtime::TaskContext;

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "task: " << message << "\n";
    }
    return condition;
}

inline TaskContext context(CompletionQueue& queue,
                           CompletionPriority priority = CompletionPriority::Visible) {
    return TaskContext{&queue, priority, {}};
}

// Drains one completion at a time so tests can observe every scheduling step.
inline std::size_t pump(CompletionQueue& queue, std::size_t budget = 32) {
    std::size_t executed = 0;
    while (!queue.empty() && executed < budget) {
        executed += queue.run(1);
    }
    return executed;
}

// The smallest value-producing child, and the smallest side-effecting one. Every test that needs to
// cross an await boundary builds on these two.
inline Task<int> doubled(TaskContext, int value) {
    co_return value * 2;
}

inline Task<void> marked(TaskContext, bool& flag) {
    flag = true;
    co_return;
}

} // namespace tessera::task_test
