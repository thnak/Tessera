// SPDX-License-Identifier: MIT

// The synchronous catch point and the FFI/thread-edge conversion.
// See docs/specs/error-boundaries-v1.md sections 1, 5 and 6.

#include "boundary_test_support.hpp"
#include "tessera/ui/boundary/guard.hpp"

#include <cstddef>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {

using tessera::ui::boundary::BoundaryCode;
using tessera::ui::boundary::BoundaryConfig;
using tessera::ui::boundary::BoundaryDisposition;
using tessera::ui::boundary::BoundaryId;
using tessera::ui::boundary::BoundaryPhase;
using tessera::ui::boundary::BoundaryTable;
using tessera::ui::boundary::CancellationSignal;
using tessera::ui::boundary::capture_current_error;
using tessera::ui::boundary::Error;
using tessera::ui::boundary::ErrorClass;
using tessera::ui::boundary::FailureSite;
using tessera::ui::boundary::GuardOutcome;
using tessera::ui::boundary::kRootBoundary;
using tessera::ui::boundary::MemoryReportSink;
using tessera::ui::boundary::PlatformFailure;
using tessera::ui::boundary::run_guarded;
using tessera::ui::boundary::test::RecordingRoot;
using tessera::ui::boundary::test::RecordingSink;
using tessera::ui::boundary::test::ThrowingSink;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "boundary_guard: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    std::size_t failures = 0;
    auto check = [&failures](bool ok, const char* message) {
        if (!expect(ok, message)) {
            ++failures;
        }
    };

    RecordingRoot root;
    MemoryReportSink diagnostics;
    BoundaryTable table(root, diagnostics);

    RecordingSink on_error;
    BoundaryConfig config;
    config.component = "guard.card";
    config.subtree = "subtree::guard";
    config.fallback = "subtree::guard.fallback";
    const auto added = table.add(config, &on_error);
    check(added.has_value(), "the guarded boundary registers");
    const BoundaryId id = added.value_or(kRootBoundary);

    // --- generated component code throws: fallback, one report --------------
    const GuardOutcome caught = run_guarded(
        table, id, FailureSite::Build, 0.0, [] { throw std::runtime_error("build failed"); }, 77);
    check(!caught.ran, "a throw means the guarded code did not run to completion");
    check(caught.failure.disposition == BoundaryDisposition::Fallback,
          "the guard converts the throw into fallback selection");
    check(caught.failure.reported && on_error.reports.size() == 1,
          "the guard reports the catch once");
    check(on_error.last() != nullptr && on_error.last()->error.message == "build failed",
          "the exception message is preserved as a value");
    check(on_error.last() != nullptr && on_error.last()->error.source_span == 77,
          "the generated source span is recorded");
    check(on_error.last() != nullptr && on_error.last()->site == FailureSite::Build,
          "the failure site is recorded");

    // --- success is not a report -------------------------------------------
    check(table.reset(id), "the boundary resets between guard cases");
    const GuardOutcome ok = run_guarded(table, id, FailureSite::Update, 1.0, [] {}, 0);
    check(ok.ran, "a successful guard run reports success");
    check(!ok.failure.reported, "a successful run produces no report");
    check(on_error.reports.size() == 1, "no report is added for a successful run");

    // --- every throw kind is classified without RTTI -----------------------
    const GuardOutcome unknown =
        run_guarded(table, id, FailureSite::Effect, 2.0, [] { throw 42; }, 0);
    check(unknown.failure.disposition == BoundaryDisposition::Fallback,
          "a non-standard exception is still caught");
    check(unknown.failure.error.code == "unknown_exception",
          "a non-standard exception is reported as unknown");
    check(table.reset(id), "the boundary resets after the unknown-exception case");

    const GuardOutcome oom =
        run_guarded(table, id, FailureSite::Build, 3.0, [] { throw std::bad_alloc{}; }, 0);
    check(oom.failure.disposition == BoundaryDisposition::Escalated,
          "a thrown bad_alloc is not caught by the boundary");
    check(oom.failure.handled_by == kRootBoundary, "OOM reaches the root handler");
    check(root.reports.size() == 1 && root.last()->error.kind == ErrorClass::Fatal,
          "the root report keeps the fatal class");

    const GuardOutcome platform =
        run_guarded(table, id, FailureSite::Paint, 4.0, [] { throw PlatformFailure{}; }, 0);
    check(platform.failure.disposition == BoundaryDisposition::Escalated,
          "a platform failure is not caught by the boundary");

    const std::size_t reports_before_cancel = on_error.reports.size();
    const GuardOutcome cancelled =
        run_guarded(table, id, FailureSite::Effect, 5.0, [] { throw CancellationSignal{}; }, 0);
    check(cancelled.failure.disposition == BoundaryDisposition::Cancelled,
          "cancellation does not show the fallback");
    check(!cancelled.failure.reported && on_error.reports.size() == reports_before_cancel,
          "cancellation is not reported as an error");
    check(table.state(id) != nullptr && table.state(id)->phase == BoundaryPhase::Normal,
          "cancellation leaves the boundary normal");

    // --- catchEvents == false still never leaks an exception ---------------
    RecordingSink strict_sink;
    BoundaryConfig strict = config;
    strict.component = "guard.strict";
    strict.catch_events = false;
    const auto strict_added = table.add(strict, &strict_sink);
    const BoundaryId strict_id = strict_added.value_or(kRootBoundary);
    const GuardOutcome strict_event = run_guarded(
        table, strict_id, FailureSite::Event, 6.0, [] { throw std::runtime_error("click"); }, 0);
    check(strict_event.failure.disposition == BoundaryDisposition::Escalated,
          "catchEvents=false escalates an event throw to the root handler");
    check(strict_sink.reports.empty(), "the strict boundary shows no fallback");

    // --- the FFI/thread edge converts instead of throwing ------------------
    const std::size_t root_before_capture = root.reports.size();
    Error captured;
    try {
        throw std::runtime_error("bridge callback");
    } catch (...) {
        captured = capture_current_error();
    }
    check(captured.kind == ErrorClass::Recoverable,
          "a foreign exception becomes a recoverable value");
    check(captured.code == "exception" && captured.message == "bridge callback",
          "the bridge conversion keeps the message");
    check(root.reports.size() == root_before_capture,
          "converting an exception at the edge does not report by itself");

    // --- onError that throws must not escape the catch point ---------------
    RecordingRoot throwing_root;
    MemoryReportSink throwing_diagnostics;
    BoundaryTable throwing_table(throwing_root, throwing_diagnostics);
    ThrowingSink throwing_sink;
    BoundaryConfig throwing_config = config;
    throwing_config.component = "guard.throwing";
    const auto throwing_added = throwing_table.add(throwing_config, &throwing_sink);
    const BoundaryId throwing_id = throwing_added.value_or(kRootBoundary);
    const GuardOutcome contained = run_guarded(
        throwing_table, throwing_id, FailureSite::Build, 7.0,
        [] { throw std::runtime_error("first"); }, 0);
    check(contained.failure.disposition == BoundaryDisposition::Fallback,
          "a throwing onError does not change the fallback decision");
    check(throwing_root.reports.size() == 1 &&
              throwing_root.last()->code == BoundaryCode::FallbackFailed,
          "a throwing onError is contained and reported as FALLBACK_FAILED at the root");

    if (failures != 0) {
        std::cerr << "boundary_guard: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "boundary_guard: ok\n";
    return 0;
}
