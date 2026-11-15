// SPDX-License-Identifier: MIT
//
// The service contract and its lifecycle states (ADR-050 section 4). A service is a long-lived
// app- or window-scoped object with an asynchronous start and stop; AF1 implements the state
// machine and the ordered, deadline-bounded sequencing, not the app lifecycle that will drive it.

#pragma once

#include "tessera/appfw/service_context.hpp"
#include "tessera/appfw/service_id.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"

#include <cstdint>
#include <span>

namespace tessera::appfw {

// ADR-050 section 4.1 state diagram:
//
//   Registered -> Starting -> Started -> Stopping -> Stopped
//                     \           \          \
//                      +-> Failed  +-> Failed +-> Abandoned (deadline expired)
//
// `Failed` is also the state of a service whose dependency failed and which therefore never ran;
// `Abandoned` is the only terminal state a stop sequence can produce when a service overruns its
// deadline (its stop task is cancelled and its diagnostics are recorded).
enum class ServiceState : std::uint8_t {
    Registered,
    Starting,
    Started,
    Stopping,
    Stopped,
    Failed,
    Abandoned,
};

[[nodiscard]] constexpr const char* service_state_name(ServiceState state) noexcept {
    switch (state) {
    case ServiceState::Registered:
        return "registered";
    case ServiceState::Starting:
        return "starting";
    case ServiceState::Started:
        return "started";
    case ServiceState::Stopping:
        return "stopping";
    case ServiceState::Stopped:
        return "stopped";
    case ServiceState::Failed:
        return "failed";
    case ServiceState::Abandoned:
        return "abandoned";
    }
    return "unknown";
}

// ADR-050 writes this as `Status`. It is spelled `ServiceStatus` here so an app framework header
// does not put the bare name `Status` in `tessera::appfw`.
enum class ServiceStatus : std::uint8_t {
    Ok,
    Failed,
};

// Everything a `start()` needs. `task` is the scheduling context (queue, priority, cancellation
// token) the service's own coroutine must be created with.
//
// Implementation note (AF1), also recorded in ADR-050: the literal form
//
//   runtime::Task<ServiceStatus> start(StartContext& ctx) override { co_await ...; }
//
// is NOT expressible with the in-tree `runtime::Task`. `Task::promise_type`'s constructor takes the
// scheduling context as its *first* argument, and a member coroutine's implicit object parameter
// (`*this`) already occupies that position, so the promise constructor is never matched and the
// member body cannot be a coroutine at all (verified with MSVC 14.44).
//
// The shape that works is the free-body form: the override returns the coroutine, and a
// context-first free or static function is the coroutine. That is what `ServiceRunner` does
// internally, and what an implementation must write:
//
//   runtime::Task<ServiceStatus> start(StartContext& ctx) override {
//       return run_start(ctx, *this);
//   }
//   static runtime::Task<ServiceStatus> run_start(StartContext ctx, MailService& self) {
//       MailStore& store = *ctx.services->find<MailStore>();
//       co_await store.load();
//       co_return ServiceStatus::Ok;
//   }
//
// The conversion operator below is what lets that coroutine's promise take the context directly
// from the `StartContext` parameter, so a service never has to pass the queue by hand.
struct StartContext {
    runtime::TaskContext task;
    // Frozen scope the service may resolve its own dependencies from (ADR-050 section 3 item 5
    // allows lookups in `start()`). The scope outlives the start sequence.
    const ServiceContext* services = nullptr;
    // Absolute deadline on the runner's clock. A start that reaches it is cancelled and Failed.
    double deadline_seconds = 0.0;

    operator runtime::TaskContext() const noexcept {
        return task;
    }
};

// Everything a `stop()` needs. There is deliberately no `services` member: a service must not
// resolve new dependencies while shutting down.
struct StopContext {
    runtime::TaskContext task;
    // Absolute deadline on the runner's clock. A stop that reaches it is cancelled and Abandoned.
    double deadline_seconds = 0.0;

    operator runtime::TaskContext() const noexcept {
        return task;
    }
};

// Virtual dispatch at the service boundary is the ADR-050 section 4 item 6 decision; per-frame
// calls must go through a cached pointer to a `final` type instead.
class Service {
public:
    Service() = default;
    virtual ~Service() = default;

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    virtual runtime::Task<ServiceStatus> start(StartContext& ctx) = 0;
    virtual runtime::Task<void> stop(StopContext& ctx) = 0;
};

// What the composition root declares about one service. `depends_on` is borrowed only for the
// duration of the call: the runner copies it into fixed storage.
struct ServiceRegistration {
    static constexpr std::size_t kMaxDependencies = 8;

    ServiceId id{};
    std::span<const ServiceId> depends_on{};
    // A required service gates app readiness (P4 in ADR-050 section 4 item 2). AF1 records the flag
    // and its failures; routing a required failure to the root error boundary belongs to AF3.
    bool required = false;
};

} // namespace tessera::appfw
