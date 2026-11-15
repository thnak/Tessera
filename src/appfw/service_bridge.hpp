// SPDX-License-Identifier: MIT
//
// The bridge between a service coroutine and the runner (ADR-050 section 4 item 5: an exception
// escaping `start`/`stop` is caught at the service boundary, turned into a status value and never
// reaches the frame loop).
//
// A bridge is one driver coroutine that creates the service's task, awaits it and reports the
// outcome through a plain function pointer. Three consequences shape the runner:
//   * the driver's own `TaskContext` is deliberately token-free. `Task::post_resume` checks the
//   token
//     before resuming, so a tokened driver would be skipped exactly when its service is cancelled
//     and the state machine would hang waiting for a report that never comes. The token belongs to
//     the service task the driver awaits;
//   * the report is delivered from inside the completion-queue drain, so the next transition never
//     runs inline on the stack that called `begin_start()`/`begin_stop()`;
//   * the driver frame owns the `StartContext`/`StopContext` the service sees, because they are
//     parameters. They therefore stay valid for as long as the service coroutine can run.

#pragma once

#include "tessera/appfw/service.hpp"
#include "tessera/runtime/task.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::appfw::detail {

// Non-owning sink: a plain function pointer plus the runner, so a transition allocates nothing.
struct ReportSink {
    void* owner = nullptr;
    void (*deliver)(void* owner, std::size_t slot, std::uint32_t generation,
                    ServiceStatus status) = nullptr;
};

// One start or stop attempt. Holding the driver task keeps the coroutine frame alive; the frame
// owns the service's context, so it must outlive the service coroutine (it does: the driver awaits
// it).
struct Bridge {
    runtime::Task<void> driver;
};

// `context` is the driver's own scheduling context and must be token-free; the service's task takes
// its context (with the cancellation token) from `start.task` / `stop.task`.
[[nodiscard]] runtime::Task<void> drive_start(runtime::TaskContext context, Service& service,
                                              StartContext start, ReportSink sink, std::size_t slot,
                                              std::uint32_t generation);

[[nodiscard]] runtime::Task<void> drive_stop(runtime::TaskContext context, Service& service,
                                             StopContext stop, ReportSink sink, std::size_t slot,
                                             std::uint32_t generation);

} // namespace tessera::appfw::detail
