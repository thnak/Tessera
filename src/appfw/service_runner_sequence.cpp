// SPDX-License-Identifier: MIT
//
// The start/stop sequence: one service in flight at a time, advanced by completion-queue reports,
// bounded by per-service and total deadlines that only `tick()` evaluates.
//
// Ordering rules, each pinned to ADR-050 section 4:
//   * item 1 — start in topological order, stop in reverse order;
//   * the diagram — `Registered -> Starting -> Started -> Stopping -> Stopped`, with `Failed` out
//   of
//     Starting and `Abandoned` out of Stopping when the stop deadline expires;
//   * item 4 — a service past its deadline is cancelled and marked Abandoned and shutdown
//   continues.
// Two readings of underspecified transitions, both conservative:
//   * only a service that reached `Started` is stopped. A service whose start failed cleans up
//   after
//     itself, because stopping something that never started has no defined meaning in the diagram.
//   * a stop that throws is `Abandoned`, not `Stopped`: the diagram gives Stopping only those two
//     outcomes, and an unconfirmed stop is not a confirmed one.

#include "service_runner_impl.hpp"
#include "tessera/runtime/task.hpp"

#include <utility>

namespace tessera::appfw {

namespace {

[[nodiscard]] double deadline_after(double now, double budget) noexcept {
    if (!(budget > 0.0)) {
        return detail::RunnerState::kInfinite;
    }
    return now + budget;
}

[[nodiscard]] double earlier(double left, double right) noexcept {
    return left < right ? left : right;
}

// Trampolines: the sink carries a plain function pointer, so a transition allocates nothing and no
// `std::function` indirection is on the path.
void deliver_start(void* owner, std::size_t slot, std::uint32_t generation, ServiceStatus status) {
    static_cast<detail::RunnerState*>(owner)->on_start_report(slot, generation, status);
}

void deliver_stop(void* owner, std::size_t slot, std::uint32_t generation, ServiceStatus status) {
    static_cast<detail::RunnerState*>(owner)->on_stop_report(slot, generation, status);
}

} // namespace

bool detail::RunnerState::publish_start(std::size_t slot) {
    Slot& entry = slots[slot];
    ++entry.generation;
    entry.state = ServiceState::Starting;
    entry.cancel = runtime::CancellationSource{};
    entry.deadline =
        earlier(deadline_after(clock->now_seconds(), config.per_service_start_deadline_seconds),
                sequence_deadline);

    StartContext start;
    start.services = services;
    start.deadline_seconds = entry.deadline;
    // The service's task takes its context (queue, priority, cancellation token) from here.
    start.task = runtime::TaskContext{completions, config.priority, entry.cancel.token()};

    auto fresh = std::make_unique<detail::Bridge>();
    detail::ReportSink sink{this, &deliver_start};
    // The driver runs on a token-free context on purpose: `Task::post_resume` skips a task whose
    // token is cancelled, so a tokened driver would be skipped exactly when its service is
    // cancelled and the state machine would wait forever for a report that can never arrive.
    const runtime::TaskContext driver{completions, config.priority, {}};
    fresh->driver =
        detail::drive_start(driver, *entry.service, start, sink, slot, entry.generation);

    inflight = slot;
    if (!fresh->driver.start()) {
        // The queue refused the first resume, so the service body never ran and nothing leaked.
        inflight = kNoSlot;
        return false;
    }
    bridge = std::move(fresh);
    return true;
}

bool detail::RunnerState::publish_stop(std::size_t slot) {
    Slot& entry = slots[slot];
    ++entry.generation;
    entry.state = ServiceState::Stopping;
    entry.cancel = runtime::CancellationSource{};
    entry.deadline =
        earlier(deadline_after(clock->now_seconds(), config.per_service_stop_deadline_seconds),
                sequence_deadline);

    StopContext stop;
    stop.deadline_seconds = entry.deadline;
    stop.task = runtime::TaskContext{completions, config.priority, entry.cancel.token()};

    auto fresh = std::make_unique<detail::Bridge>();
    detail::ReportSink sink{this, &deliver_stop};
    const runtime::TaskContext driver{completions, config.priority, {}};
    fresh->driver = detail::drive_stop(driver, *entry.service, stop, sink, slot, entry.generation);

    inflight = slot;
    if (!fresh->driver.start()) {
        inflight = kNoSlot;
        return false;
    }
    bridge = std::move(fresh);
    return true;
}

void detail::RunnerState::mark_start_failed(std::size_t slot) {
    Slot& entry = slots[slot];
    entry.state = ServiceState::Failed;
    ++stats.failed;
    if (entry.required) {
        required_failure = true;
    }
    // Direct dependents cannot start either. Transitive dependents are handled when the cursor
    // reaches them, because it walks the order in topological sequence.
    for (std::size_t index = 0; index < registered; ++index) {
        Slot& candidate = slots[index];
        if (candidate.state != ServiceState::Registered) {
            continue;
        }
        for (std::size_t dependency = 0; dependency < candidate.dependency_count; ++dependency) {
            if (candidate.dependency_slots[dependency] == slot) {
                candidate.state = ServiceState::Failed;
                ++stats.failed;
                if (candidate.required) {
                    required_failure = true;
                }
                break;
            }
        }
    }
}

void detail::RunnerState::finish_start() {
    inflight = kNoSlot;
    next = 0;
    phase = required_failure ? ServicePhase::Failed : ServicePhase::Running;
}

void detail::RunnerState::advance_start() {
    while (next < order_size) {
        const std::size_t slot = order_slots[next];
        if (slots[slot].state == ServiceState::Registered) {
            if (budget_exhausted()) {
                // Same reasoning as in `advance_stop`: no attempt can finish inside a budget that
                // is already gone, so the remaining services are recorded as never started.
                mark_start_failed(slot);
                ++next;
                continue;
            }
            if (publish_start(slot)) {
                // The sequence continues from the report; nothing else runs on this stack.
                return;
            }
            // The queue refused the driver, so the service cannot start at all.
            mark_start_failed(slot);
        }
        ++next;
    }
    finish_start();
}

void detail::RunnerState::advance_stop() {
    while (next > 0) {
        --next;
        const std::size_t slot = order_slots[next];
        if (slots[slot].state != ServiceState::Started) {
            // Never started, or already finalized: nothing to stop and nothing to report.
            continue;
        }
        if (budget_exhausted()) {
            // The total budget is gone, so no attempt could finish inside it. Starting one anyway
            // would exceed the budget or hand the service a deadline that has already passed; both
            // are worse than recording that it could not be stopped in time.
            slots[slot].state = ServiceState::Abandoned;
            ++stats.abandoned;
            continue;
        }
        if (publish_stop(slot)) {
            return;
        }
        // A stop that never got to run cannot be confirmed, so the service is Abandoned rather than
        // Stopped; shutdown still continues.
        slots[slot].state = ServiceState::Abandoned;
        ++stats.abandoned;
    }
    inflight = kNoSlot;
    phase = ServicePhase::Stopped;
}

void detail::RunnerState::on_start_report(std::size_t slot, std::uint32_t generation,
                                          ServiceStatus status) {
    Slot& entry = slots[slot];
    if (entry.generation != generation || entry.state != ServiceState::Starting) {
        // A late report from an abandoned attempt is dropped (ADR-050 section 4 item 4).
        return;
    }
    inflight = kNoSlot;
    if (status == ServiceStatus::Ok) {
        entry.state = ServiceState::Started;
        ++stats.started;
    } else {
        mark_start_failed(slot);
    }
    ++next;
    // The finished bridge is released here, one transition after its driver reported, so a driver
    // frame is never destroyed while it is still running.
    bridge.reset();
    if (phase == ServicePhase::Starting) {
        advance_start();
    }
}

void detail::RunnerState::on_stop_report(std::size_t slot, std::uint32_t generation,
                                         ServiceStatus status) {
    Slot& entry = slots[slot];
    if (entry.generation != generation || entry.state != ServiceState::Stopping) {
        return;
    }
    inflight = kNoSlot;
    if (status == ServiceStatus::Ok) {
        entry.state = ServiceState::Stopped;
        ++stats.stopped;
    } else {
        entry.state = ServiceState::Abandoned;
        ++stats.abandoned;
    }
    bridge.reset();
    if (phase == ServicePhase::Stopping) {
        advance_stop();
    }
}

void detail::RunnerState::abandon(std::size_t slot, ServiceState terminal) {
    Slot& entry = slots[slot];
    // Cancelling only drops the service's token: a cooperative coroutine unwinds at its next
    // resume, and the generation bump makes any late report a no-op.
    entry.cancel.cancel();
    ++entry.generation;
    inflight = kNoSlot;
    if (bridge != nullptr) {
        // The service coroutine may still be suspended inside this frame, so it cannot be destroyed
        // here: the `StartContext`/`StopContext` it refers to live in that frame. Orphans are
        // released with the runner, at app teardown, which is the only point where no further
        // resume can run. A service that ignores its token keeps its frame alive; the abandonment
        // counters report that rather than the runner pretending the frame is gone.
        orphans.push_back(std::move(bridge));
    }
    if (terminal == ServiceState::Abandoned) {
        entry.state = ServiceState::Abandoned;
        ++stats.abandoned;
        return;
    }
    mark_start_failed(slot);
}

bool detail::RunnerState::budget_exhausted() const noexcept {
    return sequence_deadline < kInfinite && clock->now_seconds() >= sequence_deadline;
}

bool detail::RunnerState::begin_start() {
    if (!prepared || phase != ServicePhase::Idle) {
        return false;
    }
    phase = ServicePhase::Starting;
    required_failure = false;
    next = 0;
    sequence_deadline = deadline_after(clock->now_seconds(), config.total_start_deadline_seconds);
    advance_start();
    return true;
}

bool detail::RunnerState::begin_stop() {
    if (phase != ServicePhase::Running && phase != ServicePhase::Failed) {
        return false;
    }
    phase = ServicePhase::Stopping;
    next = order_size;
    sequence_deadline = deadline_after(clock->now_seconds(), config.total_stop_deadline_seconds);
    advance_stop();
    return true;
}

void detail::RunnerState::tick() {
    if (inflight == kNoSlot) {
        return;
    }
    const std::size_t slot = inflight;
    if (clock->now_seconds() < slots[slot].deadline) {
        return;
    }
    if (phase == ServicePhase::Starting) {
        // A start past its deadline is Failed (the diagram's Starting -> Failed edge), and its
        // dependents cannot start; the sequence continues with the services that do not need it.
        abandon(slot, ServiceState::Failed);
        ++next;
        advance_start();
        return;
    }
    if (phase == ServicePhase::Stopping) {
        abandon(slot, ServiceState::Abandoned);
        advance_stop();
    }
}

} // namespace tessera::appfw
