// SPDX-License-Identifier: MIT
//
// Private state of `ServiceRunner`. The public header only forward-declares `ServiceRunner::Impl`,
// so this definition is what the two implementation files agree on. It sits at namespace scope
// (named `RunnerState`) rather than inside the class because out-of-line members of a private
// nested type are not accessible from the free functions that implement the sequence.

#pragma once

#include "service_bridge.hpp"
#include "tessera/appfw/service.hpp"
#include "tessera/appfw/service_context.hpp"
#include "tessera/appfw/service_id.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace tessera::appfw {

namespace detail {

struct RunnerState {
    // A config deadline of 0 (or less) means "no deadline"; the arithmetic spells that as infinite.
    static constexpr double kInfinite = 1.0e300;
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    struct Slot {
        std::uint64_t id = 0;
        Service* service = nullptr;
        ServiceState state = ServiceState::Registered;
        bool required = false;
        // Bumped on every start/stop attempt; a report with a stale generation is dropped.
        std::uint32_t generation = 0;
        double deadline = kInfinite;
        std::size_t dependency_count = 0;
        std::uint64_t dependencies[ServiceRegistration::kMaxDependencies] = {};
        // Resolved during prepare(), so the sequence never searches again.
        std::size_t dependency_slots[ServiceRegistration::kMaxDependencies] = {};
        runtime::CancellationSource cancel;
    };

    RunnerState(runtime::Clock& clock, runtime::CompletionQueue& completions,
                const ServiceContext* services, ServiceRunnerConfig config);

    [[nodiscard]] std::size_t find_slot(ServiceId id) const noexcept;

    // Registration and ordering (service_runner.cpp).
    std::expected<void, ServiceError> add(const ServiceRegistration& registration,
                                          Service& service);
    std::expected<void, ServiceError> prepare();
    bool begin_start();
    bool begin_stop();
    void tick();

    // The sequence (service_runner_sequence.cpp).
    void advance_start();
    void advance_stop();
    void on_start_report(std::size_t slot, std::uint32_t generation, ServiceStatus status);
    void on_stop_report(std::size_t slot, std::uint32_t generation, ServiceStatus status);
    void mark_start_failed(std::size_t slot);
    // Publishes one attempt. Returns false when the queue rejected it; the caller then finalizes
    // the slot, so the sequence never recurses from inside publish.
    bool publish_start(std::size_t slot);
    bool publish_stop(std::size_t slot);
    // Cancels the in-flight attempt and finalizes the slot. Its bridge is retained, not destroyed:
    // a cancelled service's coroutine may still be suspended (see `orphans`).
    void abandon(std::size_t slot, ServiceState terminal);
    void finish_start();
    // True once the current sequence's total budget has expired. A sequence with no total budget
    // (`kInfinite`) is never exhausted.
    [[nodiscard]] bool budget_exhausted() const noexcept;

    runtime::Clock* clock = nullptr;
    runtime::CompletionQueue* completions = nullptr;
    // The frozen scope a start() may resolve its own dependencies from (ADR-050 section 3 item 5).
    const ServiceContext* services = nullptr;
    ServiceRunnerConfig config;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<bool[]> placed;
    // Start order (dependencies first): `order_ids` is what `start_order()` exposes, `order_slots`
    // is the slot each id resolved to.
    std::unique_ptr<ServiceId[]> order_ids;
    std::unique_ptr<std::size_t[]> order_slots;
    std::size_t registered = 0;
    std::size_t order_size = 0;
    // Cursor over `order_slots`: forward while starting, backward while stopping.
    std::size_t next = 0;
    std::size_t inflight = kNoSlot;
    // The attempt in flight. Released one transition after it reports, so its driver frame is never
    // destroyed while that frame is still running.
    std::unique_ptr<detail::Bridge> bridge;
    // Attempts abandoned at a deadline. Cancellation is cooperative, so the service coroutine may
    // still be suspended on external work inside these frames; destroying one would destroy the
    // `StartContext`/`StopContext` its coroutine still refers to. They are therefore released at
    // runner destruction (app teardown), the only point where no further resume can run. A service
    // that ignores its token keeps a frame alive: that is a service bug, reported through the
    // abandonment counters rather than papered over.
    std::vector<std::unique_ptr<detail::Bridge>> orphans;
    ServicePhase phase = ServicePhase::Idle;
    bool prepared = false;
    bool required_failure = false;
    double sequence_deadline = kInfinite;
    ServiceRunnerStats stats{};
};

} // namespace detail

} // namespace tessera::appfw
