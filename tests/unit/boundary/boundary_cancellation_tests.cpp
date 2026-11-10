// SPDX-License-Identifier: MIT

// Cancellation reconciliation: a cancellation value stays silent, a cancellation signal caught as
// an exception reaches the root handler exactly once as a non-error notification.
// See docs/specs/error-boundaries-v1.md sections 1, 6, 7 and 11.

#include "boundary_test_support.hpp"
#include "tessera/ui/boundary/guard.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

using tessera::ui::boundary::BoundaryCode;
using tessera::ui::boundary::BoundaryConfig;
using tessera::ui::boundary::BoundaryDisposition;
using tessera::ui::boundary::BoundaryId;
using tessera::ui::boundary::BoundaryPhase;
using tessera::ui::boundary::BoundaryTable;
using tessera::ui::boundary::CancellationSignal;
using tessera::ui::boundary::CatchOutcome;
using tessera::ui::boundary::ErrorClass;
using tessera::ui::boundary::FailureSite;
using tessera::ui::boundary::GuardOutcome;
using tessera::ui::boundary::kRootBoundary;
using tessera::ui::boundary::make_error;
using tessera::ui::boundary::MemoryReportSink;
using tessera::ui::boundary::RetryState;
using tessera::ui::boundary::run_guarded;
using tessera::ui::boundary::test::RecordingOwner;
using tessera::ui::boundary::test::RecordingRoot;
using tessera::ui::boundary::test::RecordingSink;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "boundary_cancellation: " << message << "\n";
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
    RecordingOwner owner;
    RecordingSink on_error;

    BoundaryConfig config;
    config.component = "cancel.card";
    config.subtree = "subtree::cancel";
    config.fallback = "subtree::cancel.fallback";
    config.max_auto_retries = 2;
    const auto added = table.add(config, &on_error, &owner);
    check(added.has_value(), "the cancellation boundary registers");
    const BoundaryId id = added.value_or(kRootBoundary);
    check(table.cancellation_notifications() == 0, "the diagnostics counter starts at zero");

    // --- an exception-path cancellation notifies the root exactly once ------
    const std::size_t root_before = root.reports.size();
    const std::size_t on_error_before = on_error.reports.size();
    const std::size_t caught_before = diagnostics.count(BoundaryCode::Caught);
    const GuardOutcome caught =
        run_guarded(table, id, FailureSite::Effect, 3.0, [] { throw CancellationSignal{}; }, 91);
    check(!caught.ran, "the guarded run stops at the cancellation");
    check(caught.failure.disposition == BoundaryDisposition::Cancelled,
          "a cancellation signal is a control outcome, not a fallback");
    check(!caught.failure.reported, "cancellation is not an error report");
    check(on_error.reports.size() == on_error_before, "onError is never called for cancellation");
    check(diagnostics.count(BoundaryCode::Caught) == caught_before,
          "cancellation is not counted as a caught error");

    check(root.reports.size() == root_before + 1, "the root handler is notified exactly once");
    check(root.last() != nullptr && root.last()->code == BoundaryCode::Cancelled,
          "the notification carries TESSERA_BOUNDARY_CANCELLED");
    check(root.last() != nullptr && root.last()->error.kind == ErrorClass::Cancellation,
          "the notification keeps the non-error cancellation classification");
    check(root.last() != nullptr && root.last()->boundary == kRootBoundary,
          "the notification comes from the root handler, not a component boundary");
    check(root.last() != nullptr && root.last()->component == "cancel.card",
          "the notification names the component that raised it");
    check(root.last() != nullptr && root.last()->site == FailureSite::Effect &&
              root.last()->error.source_span == 91,
          "the notification records the failure site and source span");
    check(root.last() != nullptr && root.last()->error.message == "cancelled",
          "the marker message survives as a value");
    check(diagnostics.count(BoundaryCode::Cancelled) == 0,
          "the notification bypasses the diagnostics sink; the counter is the diagnostics surface");
    check(table.cancellation_notifications() == 1,
          "the diagnostics counter records one cancellation notification");

    const auto* state = table.state(id);
    check(state != nullptr && state->phase == BoundaryPhase::Normal,
          "cancellation leaves the boundary normal");
    check(state != nullptr && state->error.code.empty() && !state->fallback_live &&
              !state->child_detached,
          "cancellation stores no error and detaches nothing");
    check(owner.disposed.empty(), "cancellation disposes no subtree");
    check(caught.failure.retry.state == RetryState::Idle && state != nullptr && state->retries == 0,
          "cancellation does not schedule or spend a retry");
    check(table.advance(id, 100.0).state == RetryState::Idle,
          "the retry pump stays idle after a cancellation");

    // --- one notification per catch, never a silent second one -------------
    const std::size_t root_after_first = root.reports.size();
    const GuardOutcome caught_again =
        run_guarded(table, id, FailureSite::Update, 4.0, [] { throw CancellationSignal{}; }, 0);
    check(caught_again.failure.disposition == BoundaryDisposition::Cancelled,
          "a repeated cancellation is still a control outcome");
    check(root.reports.size() == root_after_first + 1,
          "each caught cancellation notifies the root handler exactly once");
    check(table.cancellation_notifications() == 2, "the counter is monotonic per catch");

    // --- a cancellation value is not a notification ------------------------
    const std::size_t root_before_value = root.reports.size();
    const CatchOutcome cancelled_value =
        table.report_async(id, 0, make_error(CancellationSignal{}), 5.0);
    check(cancelled_value.disposition == BoundaryDisposition::Cancelled,
          "an async cancellation value does not show a fallback");
    check(root.reports.size() == root_before_value,
          "an async cancellation value never notifies the root handler");
    check(table.cancellation_notifications() == 2,
          "the counter only counts exception-path cancellation signals");
    check(table.state(id) != nullptr && table.state(id)->phase == BoundaryPhase::Normal,
          "an async cancellation value leaves the boundary normal");

    // --- a recoverable error is still an error report, not a notification --
    const std::size_t root_before_error = root.reports.size();
    const GuardOutcome failed = run_guarded(
        table, id, FailureSite::Build, 6.0, [] { throw std::runtime_error("boom"); }, 0);
    check(failed.failure.disposition == BoundaryDisposition::Fallback,
          "a recoverable throw still shows the fallback");
    check(failed.failure.reported && on_error.count(BoundaryCode::Caught) == 1,
          "a recoverable throw is reported as CAUGHT exactly once");
    check(root.reports.size() == root_before_error,
          "a caught recoverable error is not a root notification");
    check(table.cancellation_notifications() == 2,
          "the cancellation counter ignores recoverable errors");
    check(owner.count("subtree::cancel") == 1, "the failed child is disposed once");

    // --- cancellation while the fallback is live keeps the fallback --------
    const std::size_t root_before_live = root.reports.size();
    const GuardOutcome live =
        run_guarded(table, id, FailureSite::Event, 7.0, [] { throw CancellationSignal{}; }, 0);
    check(live.failure.disposition == BoundaryDisposition::Cancelled,
          "cancellation is not routed to a fallback even when one is showing");
    check(table.state(id) != nullptr && table.state(id)->phase == BoundaryPhase::Fallback &&
              table.state(id)->fallback_live,
          "cancellation keeps the live fallback in place");
    check(root.reports.size() == root_before_live + 1 &&
              root.last()->code == BoundaryCode::Cancelled,
          "cancellation while the fallback is live still notifies the root once");
    check(table.cancellation_notifications() == 3, "the live-fallback cancellation is counted");
    check(owner.count("subtree::cancel.fallback") == 0,
          "cancellation never disposes the fallback subtree");

    // --- an unmounted boundary cannot swallow a cancellation ---------------
    const std::size_t root_before_gone = root.reports.size();
    const CatchOutcome gone = table.catch_error(
        kRootBoundary + 999, make_error(CancellationSignal{}), FailureSite::Effect, 8.0);
    check(gone.disposition == BoundaryDisposition::Cancelled,
          "a cancellation from an unmounted boundary is still a control outcome");
    check(root.reports.size() == root_before_gone + 1,
          "an unmounted boundary cannot swallow the cancellation notification");
    check(table.cancellation_notifications() == 4, "the unmounted notification is counted");

    // --- reset restores the boundary without touching the counter ----------
    check(table.reset(id), "the boundary resets after the cancellation cases");
    check(table.state(id) != nullptr && table.state(id)->phase == BoundaryPhase::Normal,
          "reset reopens the boundary");
    check(table.cancellation_notifications() == 4,
          "reset does not clear the lifetime diagnostics counter");

    if (failures != 0) {
        std::cerr << "boundary_cancellation: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "boundary_cancellation: ok\n";
    return 0;
}
