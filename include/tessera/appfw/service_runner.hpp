// SPDX-License-Identifier: MIT
//
// The ordered, deadline-bounded service state machine (ADR-050 section 4 items 1 to 5).
//
// AF1 scope, stated explicitly because later slices build on it:
//   * starts run one service at a time, in topological order (dependencies first);
//   * stops run one service at a time, in the exact reverse of that order;
//   * progression is driven by the `CompletionQueue`: a per-service bridge coroutine posts the
//     result and the next transition runs inside that drain, never inline from `begin_*`;
//   * deadlines are checked in `tick()`, which reads the injected `Clock` and never sleeps;
//   * nothing here waits, joins or detaches. A service past its deadline is cancelled; a late
//     result is dropped by a generation check.
// Out of scope for AF1, left to AF3: running independent services concurrently, start phases
// (`Runtime`/`Background`/`Idle`), required-service gating of the launch (P4), the termination
// vote, and routing a required failure to the root error boundary. `required_failure()` is the seam
// the AF3 sequencer polls.
//
// Abandonment: cancelling a service only flips its token, so a service that awaits work which never
// completes keeps its own coroutine frames alive. Cooperative cancellation is the only exit, which
// is why a `Service` implementation must honour the token it was given.

#pragma once

#include "tessera/appfw/service.hpp"
#include "tessera/appfw/service_context.hpp"
#include "tessera/appfw/service_id.hpp"
#include "tessera/runtime/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace tessera::runtime {
class CompletionQueue;
}

namespace tessera::appfw {

namespace detail {
struct RunnerState;
}

enum class ServicePhase : std::uint8_t {
    // Nothing has started: `add()` and `prepare()` are still allowed.
    Idle,
    // The start sequence is running; at most one service start is in flight.
    Starting,
    // The start sequence finished and every service it could start reached Started.
    Running,
    // The start sequence finished with a required service in Failed/Abandoned (AF3 routes this to
    // the root error boundary; AF1 only reports it).
    Failed,
    // The stop sequence is running.
    Stopping,
    // Every started service reached Stopped or Abandoned.
    Stopped,
};

[[nodiscard]] constexpr const char* service_phase_name(ServicePhase phase) noexcept {
    switch (phase) {
    case ServicePhase::Idle:
        return "idle";
    case ServicePhase::Starting:
        return "starting";
    case ServicePhase::Running:
        return "running";
    case ServicePhase::Failed:
        return "failed";
    case ServicePhase::Stopping:
        return "stopping";
    case ServicePhase::Stopped:
        return "stopped";
    }
    return "unknown";
}

struct ServiceRunnerConfig {
    // ADR-050 section 4 item 4 pins a 500 ms per-service and a 2 s *total* budget for shutdown
    // only. It says nothing about start, and a start budget nobody specified is a good way to make
    // a test flaky later, so AF1 ships start with **no deadline** (0 = disabled). The enforcement
    // mechanism is implemented and exercised by the tests through these two fields; turning it on
    // for production is a separate ADR line, which is why start and stop are named separately here
    // rather than sharing one constant.
    double per_service_start_deadline_seconds = 0.0;
    double total_start_deadline_seconds = 0.0;
    // Stop deadlines: the two numbers the ADR does specify.
    double per_service_stop_deadline_seconds = 0.5;
    double total_stop_deadline_seconds = 2.0;
    std::size_t max_services = 32;
    runtime::CompletionPriority priority = runtime::CompletionPriority::Visible;
};

struct ServiceRunnerStats {
    std::size_t registered = 0;
    std::size_t started = 0;
    std::size_t failed = 0;
    std::size_t abandoned = 0;
    std::size_t stopped = 0;
};

// One state machine per scope: an app scope, or a window scope whose parent is the app scope.
//
// `services` is the frozen registry a `start()` resolves its own dependencies from (ADR-050 section
// 3 item 5 permits lookups in `start()`); it may be null when no service needs one. The clock, the
// queue and the context must all outlive the runner.
class ServiceRunner {
public:
    ServiceRunner(runtime::Clock& clock, runtime::CompletionQueue& completions,
                  const ServiceContext* services, ServiceRunnerConfig config = {});
    ~ServiceRunner();

    ServiceRunner(const ServiceRunner&) = delete;
    ServiceRunner& operator=(const ServiceRunner&) = delete;

    // Composition root only, before `prepare()`. A duplicate id, a full runner, more than
    // `ServiceRegistration::kMaxDependencies` dependencies, or a null service is an error.
    std::expected<void, ServiceError> add(const ServiceRegistration& registration,
                                          Service& service);

    // Resolves every dependency, sorts the start order and freezes the runner. A missing dependency
    // or a cycle is an error and leaves the runner unprepared, so nothing can start.
    std::expected<void, ServiceError> prepare();

    // Starts the sequence. False when the runner is not prepared or no longer Idle. Returns without
    // blocking: the first start is posted to the completion queue.
    bool begin_start();

    // Starts the stop sequence in reverse start order. False unless the phase is Running or Failed.
    // An in-flight start is cancelled first, so a start that never finished cannot hold shutdown.
    bool begin_stop();

    // Deadline enforcement only: call once per tick. It never sleeps, blocks or runs service code
    // (the completion queue does that), so it is safe on the UI thread.
    void tick();

    [[nodiscard]] ServicePhase phase() const noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    [[nodiscard]] bool sequence_active() const noexcept;
    [[nodiscard]] ServiceRunnerStats stats() const noexcept;
    // True once the start sequence ended with a required service Failed or Abandoned.
    [[nodiscard]] bool required_failure() const noexcept;
    // The start order (dependencies first); empty before `prepare()`.
    [[nodiscard]] std::span<const ServiceId> start_order() const noexcept;

    [[nodiscard]] ServiceState state_of(ServiceId id) const noexcept;
    [[nodiscard]] const ServiceContext* services() const noexcept {
        return services_;
    }

private:
    const ServiceContext* services_ = nullptr;
    // Defined in `src/appfw/service_runner_impl.hpp`. Holding it by pointer keeps the runtime
    // headers a sequence needs (async, task) out of this public header; only the name is exposed.
    std::unique_ptr<detail::RunnerState> impl_;
};

} // namespace tessera::appfw
