// SPDX-License-Identifier: MIT
//
// Test fixtures shared by the AF1 tests (issue #173): registered service types, cooperative fake
// services built on the ADR-050 section 4 contract, and the deterministic pump helpers.
//
// Determinism: every test drives time with `ManualClock` and runs service coroutines through a
// `CompletionQueue` drained by hand. There is no wall clock, no sleep, no thread and no network.

#pragma once

#include "tessera/appfw/service.hpp"
#include "tessera/appfw/service_id.hpp"
#include "tessera/runtime/async.hpp"

#include <coroutine>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// --- registry fixtures -------------------------------------------------------------------------
//
// Each registered type names its own id with `service_id("...")`, the reverse-DNS form ADR-050
// section 3 item 1 requires. Declared before the services below so a `start()` can resolve one.

struct FakeDependency {
    static constexpr auto kServiceId = tessera::appfw::service_id("tessera.test.FakeDependency");
    int value = 0;
};

struct Store {
    static constexpr auto kServiceId = tessera::appfw::service_id("tessera.test.Store");
    int value = 0;
};

struct Mail {
    static constexpr auto kServiceId = tessera::appfw::service_id("tessera.test.Mail");
    int value = 0;
};

struct UnknownId {
    static constexpr auto kServiceId = tessera::appfw::service_id("tessera.test.UnknownId");
};

namespace appfw_test {

using tessera::appfw::Service;
using tessera::appfw::ServiceStatus;
using tessera::appfw::StartContext;
using tessera::appfw::StopContext;
using tessera::runtime::CompletionPriority;
using tessera::runtime::CompletionQueue;
using tessera::runtime::Task;

namespace runtime = tessera::runtime;

inline int g_failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_service: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// Drains the queue. Every transition posts its next resume, so a full drain runs the sequence as
// far as it can without time advancing.
inline void pump(CompletionQueue& queue, std::size_t budget = 512) {
    std::size_t executed = 0;
    while (!queue.empty() && executed < budget) {
        executed += queue.run(1);
    }
}

// Suspends once through the injected completion queue, so a test can observe that the sequence
// makes progress only when the queue is drained.
struct QueueHop {
    CompletionQueue* queue = nullptr;
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) const {
        queue->post(CompletionPriority::Visible, [handle]() { handle.resume(); });
    }
    void await_resume() const noexcept {}
};

// Suspends and is never resumed: the only exit is cancellation plus a deadline. It holds no handle
// and posts nothing, so it can honestly tell the runtime that the frame will never run again, which
// is what lets an abandoned pair be freed instead of leaking (ADR-069). The handle is taken as a
// template parameter because the marker lives on the promise, and a type-erased
// `coroutine_handle<>` cannot reach it.
struct ParkForever {
    bool await_ready() const noexcept {
        return false;
    }
    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        runtime::detail::declare_no_resume(handle);
    }
    void await_resume() const noexcept {}
};

// --- a cooperative service ---------------------------------------------------------------------
//
// The ADR's contract is `start(StartContext&)`, but a member coroutine cannot be expressed with
// `runtime::Task` (its promise constructor takes the leading context argument, and a member
// coroutine's implicit object parameter occupies that position). Every service here uses the
// working shape: the override returns a context-first free body.
struct FakeService : Service {
    explicit FakeService(std::string_view service_name)
        : name(service_name) {}

    Task<ServiceStatus> start(StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    Task<void> stop(StopContext& ctx) override {
        return run_stop(ctx, *this);
    }

    static Task<ServiceStatus> run_start(StartContext ctx, FakeService& self) {
        self.events.push_back("start:" + self.name);
        if (self.resolve_dependency && ctx.services != nullptr) {
            // Legal per ADR-050 section 3 item 5: a start() may resolve its own dependencies, and a
            // lookup outside a FrameScope must not count as an in-frame lookup.
            self.resolved = ctx.services->find<FakeDependency>();
        }
        for (int hop = 0; hop < self.start_hops; ++hop) {
            co_await QueueHop{ctx.task.queue};
        }
        if (self.fail_start) {
            co_return ServiceStatus::Failed;
        }
        co_return ServiceStatus::Ok;
    }

    static Task<void> run_stop(StopContext ctx, FakeService& self) {
        self.events.push_back("stop:" + self.name);
        for (int hop = 0; hop < self.stop_hops; ++hop) {
            co_await QueueHop{ctx.task.queue};
        }
        co_return;
    }

    std::string name;
    std::vector<std::string> events;
    int start_hops = 0;
    int stop_hops = 0;
    bool fail_start = false;
    bool resolve_dependency = false;
    FakeDependency* resolved = nullptr;
};

// Starts fine but never finishes stopping: only the stop deadline can end it.
struct HangingStopService : Service {
    Task<ServiceStatus> start(StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    Task<void> stop(StopContext& ctx) override {
        return run_stop(ctx, *this);
    }
    static Task<ServiceStatus> run_start(StartContext, HangingStopService& self) {
        self.started = true;
        co_return ServiceStatus::Ok;
    }
    static Task<void> run_stop(StopContext, HangingStopService& self) {
        self.stop_entered = true;
        co_await ParkForever{};
        co_return;
    }
    bool started = false;
    bool stop_entered = false;
};

// Never finishes starting, so only a configured start deadline can end it.
struct HangingStartService : Service {
    Task<ServiceStatus> start(StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    Task<void> stop(StopContext& ctx) override {
        return run_stop(ctx, *this);
    }
    static Task<ServiceStatus> run_start(StartContext, HangingStartService& self) {
        self.start_entered = true;
        co_await ParkForever{};
        co_return ServiceStatus::Ok;
    }
    static Task<void> run_stop(StopContext, HangingStartService& self) {
        self.stop_entered = true;
        co_return;
    }
    bool start_entered = false;
    bool stop_entered = false;
};

// A service whose start throws, proving the service boundary converts it to Failed.
struct ThrowingService : Service {
    Task<ServiceStatus> start(StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    Task<void> stop(StopContext& ctx) override {
        return run_stop(ctx, *this);
    }
    static Task<ServiceStatus> run_start(StartContext, ThrowingService& self) {
        self.entered = true;
        throw std::runtime_error("service start exploded");
        co_return ServiceStatus::Ok;
    }
    static Task<void> run_stop(StopContext, ThrowingService&) {
        co_return;
    }
    bool entered = false;
};

} // namespace appfw_test
