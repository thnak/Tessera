// SPDX-License-Identifier: MIT
//
// The I/O executor seam (ADR-050 AF7, issue #179). Layer 0 already owns the *mechanisms*: the
// bounded `WorkerExecutor` thread pool, the bounded in-flight front end `IoExecutor`, and the
// `CompletionQueue` the UI thread drains. What a module that does blocking I/O needs is the
// *interface* it can be handed, so the module's own code never names a thread:

//   * production: `WorkerIoExecutor` (one bounded worker thread per in-flight slot, completion
//     posted to the shared `CompletionQueue`);
//   * tests: `ManualIoExecutor`, drained by hand, with no thread, no sleep and no wall clock.
//
// Why the seam lives here and not in `appfw`: ADR-050 section 1 lists executors in layer 0 and rule
// 1 says layer 0 gains seams, not concepts. An abstract executor is the same kind of thing as the
// other executors already in this directory, and a layer-1 module that invented its own would force
// every other module to depend on it. The storage module (layer 1) only consumes this header.
//
// Rules this seam keeps, inherited from ADR-021:
//   * a submission never runs the work inline: `post()` only enqueues, so a caller on the UI thread
//     cannot accidentally perform the blocking call;
//   * the completion never runs on the worker's stack: it is delivered through the queue the UI
//     thread drains, so the module's callback mutates UI state on the UI thread;
//   * nothing is detached. Every implementation owns what it starts and joins or drops it in its
//     destructor.

#pragma once

#include <cstddef>
#include <functional>
#include <utility>

namespace tessera::runtime {

// A blocking unit of work plus the callback that delivers its result back to the UI thread.
//
// `work` runs on an executor thread and must not throw and must not touch UI state. `completion`
// runs later, on the thread that drains the `CompletionQueue`; it may be empty. A completion that
// has been posted can still be dropped by a full queue, so `work` is where a module must release
// the resources it reserved for the submission.
struct IoTask {
    std::function<void()> work;
    std::function<void()> completion;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(work);
    }
};

class IoTaskExecutor {
public:
    virtual ~IoTaskExecutor() = default;

    IoTaskExecutor(const IoTaskExecutor&) = delete;
    IoTaskExecutor& operator=(const IoTaskExecutor&) = delete;

    // False for an invalid task or when the executor is at capacity/stopping. Never runs `work`
    // before returning.
    virtual bool post(IoTask task) = 0;

    // How many tasks may be queued or running at once. A module uses it to size its own bound.
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;

protected:
    IoTaskExecutor() = default;
};

} // namespace tessera::runtime
