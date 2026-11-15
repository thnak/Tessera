// SPDX-License-Identifier: MIT

#include "service_bridge.hpp"

namespace tessera::appfw::detail {

namespace {

// Both contexts are taken by value: the driver frame then owns the object the service coroutine
// holds a reference to, so the reference cannot outlive its storage.
void report(const ReportSink& sink, std::size_t slot, std::uint32_t generation,
            ServiceStatus status) {
    if (sink.deliver != nullptr) {
        sink.deliver(sink.owner, slot, generation, status);
    }
}

} // namespace

runtime::Task<void> drive_start(runtime::TaskContext, Service& service, StartContext start,
                                ReportSink sink, std::size_t slot, std::uint32_t generation) {
    // A service boundary (ADR-025): whatever escapes the service becomes a status value, so a
    // throwing service can never propagate into the frame loop. Recording the specific failure is
    // the diagnostics slice's job; the state machine only needs the verdict.
    ServiceStatus status = ServiceStatus::Failed;
    try {
        status = co_await service.start(start);
    } catch (...) {
        status = ServiceStatus::Failed;
    }
    report(sink, slot, generation, status);
    co_return;
}

runtime::Task<void> drive_stop(runtime::TaskContext, Service& service, StopContext stop,
                               ReportSink sink, std::size_t slot, std::uint32_t generation) {
    ServiceStatus status = ServiceStatus::Ok;
    try {
        co_await service.stop(stop);
    } catch (...) {
        status = ServiceStatus::Failed;
    }
    report(sink, slot, generation, status);
    co_return;
}

} // namespace tessera::appfw::detail
