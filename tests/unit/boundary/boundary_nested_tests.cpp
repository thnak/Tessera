// SPDX-License-Identifier: MIT

// Nested boundaries and async-error-as-value routing.
// See docs/specs/error-boundaries-v1.md sections 4, 6 and 8.

#include "boundary_test_support.hpp"

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
using tessera::ui::boundary::CatchOutcome;
using tessera::ui::boundary::FailureSite;
using tessera::ui::boundary::kRootBoundary;
using tessera::ui::boundary::make_error;
using tessera::ui::boundary::MemoryReportSink;
using tessera::ui::boundary::PlatformFailure;
using tessera::ui::boundary::RetryState;
using tessera::ui::boundary::test::RecordingOwner;
using tessera::ui::boundary::test::RecordingRoot;
using tessera::ui::boundary::test::RecordingSink;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "boundary_nested: " << message << "\n";
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
    RecordingSink panel_sink;
    RecordingSink card_sink;

    BoundaryConfig panel;
    panel.component = "dashboard.panel";
    panel.subtree = "subtree::panel";
    panel.fallback = "subtree::panel.fallback";
    const auto panel_id = table.add(panel, &panel_sink, &owner);
    check(panel_id.has_value(), "the outer boundary registers");
    const BoundaryId outer = panel_id.value_or(kRootBoundary);

    BoundaryConfig card;
    card.parent = outer;
    card.component = "dashboard.panel.card";
    card.subtree = "subtree::card";
    card.fallback = "subtree::card.fallback";
    const auto card_id = table.add(card, &card_sink, &owner);
    check(card_id.has_value(), "the nested boundary registers");
    const BoundaryId inner = card_id.value_or(kRootBoundary);

    // --- the nearest boundary catches and the parent is untouched ----------
    check(table.catcher(inner) == inner, "the nearest boundary is the catcher");
    const CatchOutcome inner_catch =
        table.catch_error(inner, make_error(std::runtime_error("card")), FailureSite::Build, 0.0);
    check(inner_catch.disposition == BoundaryDisposition::Fallback &&
              inner_catch.handled_by == inner,
          "the inner boundary catches its own child failure");
    check(card_sink.reports.size() == 1 && panel_sink.reports.empty(),
          "the parent boundary sees no report");
    check(table.state(outer) != nullptr && table.state(outer)->phase == BoundaryPhase::Normal,
          "the parent boundary stays normal");
    check(root.reports.empty(), "the root handler sees nothing");
    check(owner.count("subtree::card") == 1, "only the failed subtree is disposed");

    // A boundary already in Fallback hands its next failure to the nearest parent that can catch.
    check(table.catcher(inner) == outer, "a boundary in Fallback is no longer the catcher");
    const CatchOutcome promoted = table.catch_error(
        inner, make_error(std::runtime_error("card again")), FailureSite::Update, 0.0);
    check(promoted.disposition == BoundaryDisposition::Fallback && promoted.handled_by == outer,
          "the parent boundary catches the promoted failure");
    check(panel_sink.reports.size() == 1 && panel_sink.last()->code == BoundaryCode::Caught,
          "the parent reports the promoted catch");
    check(table.state(outer) != nullptr && table.state(outer)->phase == BoundaryPhase::Fallback,
          "the parent shows its own fallback");
    check(owner.count("subtree::panel") == 1, "the parent's child subtree is disposed");

    // --- fallback failure propagates as FALLBACK_FAILED --------------------
    RecordingOwner fallback_owner;
    RecordingSink fallback_sink;
    RecordingRoot fallback_root;
    MemoryReportSink fallback_diagnostics;
    BoundaryTable fb_table(fallback_root, fallback_diagnostics);

    BoundaryConfig outer_config;
    outer_config.component = "shell";
    outer_config.subtree = "subtree::shell";
    outer_config.fallback = "subtree::shell.fallback";
    const auto outer_added = fb_table.add(outer_config, &fallback_sink, &fallback_owner);
    const BoundaryId shell = outer_added.value_or(kRootBoundary);

    BoundaryConfig inner_config;
    inner_config.parent = shell;
    inner_config.component = "shell.widget";
    inner_config.subtree = "subtree::widget";
    inner_config.fallback = "subtree::widget.fallback";
    const auto inner_added = fb_table.add(inner_config, nullptr, &fallback_owner);
    const BoundaryId widget = inner_added.value_or(kRootBoundary);

    const CatchOutcome widget_catch = fb_table.catch_error(
        widget, make_error(std::runtime_error("widget")), FailureSite::Build, 0.0);
    check(widget_catch.disposition == BoundaryDisposition::Fallback,
          "the widget boundary catches its own failure");
    check(fallback_owner.count("subtree::widget") == 1, "the widget child was disposed");

    const CatchOutcome fallback_failed =
        fb_table.fallback_failed(widget, make_error(std::runtime_error("fallback")), 0.0);
    check(fallback_failed.disposition == BoundaryDisposition::Fallback &&
              fallback_failed.handled_by == shell,
          "a failing fallback is caught by the parent boundary");
    check(fallback_failed.reported, "the fallback failure is reported");
    check(fallback_sink.reports.size() == 1 &&
              fallback_sink.last()->code == BoundaryCode::FallbackFailed,
          "the parent receives FALLBACK_FAILED exactly once");
    check(fallback_owner.count("subtree::widget.fallback") == 1,
          "the failing fallback subtree is disposed");
    check(fallback_root.reports.empty(),
          "the root handler is not reached while the parent can catch");

    // With no ancestor left, the fallback failure goes to the root handler.
    const CatchOutcome root_fallback =
        fb_table.fallback_failed(shell, make_error(std::runtime_error("shell fallback")), 0.0);
    check(root_fallback.disposition == BoundaryDisposition::Escalated &&
              fallback_root.reports.size() == 1 &&
              fallback_root.last()->code == BoundaryCode::FallbackFailed,
          "a top-level fallback failure reaches the root handler");

    // --- async errors are values, not exceptions ---------------------------
    RecordingOwner async_owner;
    RecordingSink async_sink;
    RecordingRoot async_root;
    MemoryReportSink async_diagnostics;
    BoundaryTable async_table(async_root, async_diagnostics);

    BoundaryConfig async_config;
    async_config.component = "data.view";
    async_config.subtree = "subtree::view";
    async_config.fallback = "subtree::view.fallback";
    const auto async_added = async_table.add(async_config, &async_sink, &async_owner);
    const BoundaryId view = async_added.value_or(kRootBoundary);

    const CatchOutcome delivered =
        async_table.report_async(view, 0, make_error(std::runtime_error("load failed")), 0.0);
    check(delivered.disposition == BoundaryDisposition::Fallback && delivered.handled_by == view,
          "an async error value shows the owning boundary's fallback");
    check(delivered.retry.state == RetryState::Idle && !delivered.retry.loop,
          "an async error never consumes the auto-retry budget");
    check(async_sink.reports.size() == 1 && async_sink.last()->code == BoundaryCode::Async,
          "the async delivery reports TESSERA_BOUNDARY_ASYNC on success");
    check(async_owner.count("subtree::view") == 1, "the async failure disposes the child subtree");

    // Cancellation of an async task is not an error.
    const std::size_t async_reports_before = async_sink.reports.size();
    const std::size_t root_before = async_root.reports.size();
    const CatchOutcome async_cancelled = async_table.report_async(
        view, 0, make_error(tessera::ui::boundary::CancellationSignal{}), 0.0);
    check(async_cancelled.disposition == BoundaryDisposition::Cancelled,
          "async cancellation does not show a fallback");
    check(async_sink.reports.size() == async_reports_before &&
              async_root.reports.size() == root_before,
          "async cancellation is not reported as an error");
    check(async_table.cancellation_notifications() == 0,
          "an async cancellation value is not a cancellation notification");

    // Stale generation: the child was rebuilt, so a late completion is dropped with a diagnostic.
    async_table.reset(view);
    const std::size_t stale_before = async_diagnostics.count(BoundaryCode::Async);
    const CatchOutcome stale =
        async_table.report_async(view, 0, make_error(std::runtime_error("stale")), 0.0);
    check(stale.disposition == BoundaryDisposition::Dropped && stale.dropped,
          "a stale generation is dropped");
    check(async_sink.reports.size() == async_reports_before, "a stale value never calls onError");
    check(async_diagnostics.count(BoundaryCode::Async) == stale_before + 1,
          "a stale value is recorded as a diagnostic");
    check(async_diagnostics.reports().back().dropped, "the diagnostic marks the value dropped");

    // A completion for an unmounted boundary is dropped as well (no stale callback).
    const CatchOutcome gone = async_table.report_async(kRootBoundary + 999, 0,
                                                       make_error(std::runtime_error("gone")), 0.0);
    check(gone.disposition == BoundaryDisposition::Dropped,
          "a report for a missing boundary is dropped");

    // A catastrophic async value still bypasses the boundary and reaches the root handler.
    const CatchOutcome async_fatal =
        async_table.report_async(view, 1, make_error(PlatformFailure{}), 0.0);
    check(async_fatal.disposition == BoundaryDisposition::Escalated &&
              async_root.reports.size() == root_before + 1,
          "an async platform failure goes to the root handler");
    check(async_fatal.handled_by == kRootBoundary, "the root handler is the last catch point");

    // --- unmounting a parent unmounts the subtree --------------------------
    check(table.remove(outer) == 2, "unmounting a parent unmounts its descendants");
    check(!table.contains(outer) && !table.contains(inner), "the whole subtree is gone");
    check(owner.count("subtree::panel") == 1, "the parent child subtree is not disposed twice");

    if (failures != 0) {
        std::cerr << "boundary_nested: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "boundary_nested: ok\n";
    return 0;
}
