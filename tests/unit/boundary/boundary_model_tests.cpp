// SPDX-License-Identifier: MIT

// Error-boundary model: fallback selection, catastrophic routing, catchEvents gating, reset,
// redaction, and cleanup. See docs/specs/error-boundaries-v1.md sections 1, 2, 4, 7 and 8.

#include "boundary_test_support.hpp"
#include "tessera/ui/boundary/guard.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using tessera::ui::boundary::BoundaryCode;
using tessera::ui::boundary::BoundaryConfig;
using tessera::ui::boundary::BoundaryDisposition;
using tessera::ui::boundary::BoundaryId;
using tessera::ui::boundary::BoundaryPhase;
using tessera::ui::boundary::BoundaryState;
using tessera::ui::boundary::BoundaryTable;
using tessera::ui::boundary::CancellationSignal;
using tessera::ui::boundary::CatchOutcome;
using tessera::ui::boundary::Error;
using tessera::ui::boundary::FailureSite;
using tessera::ui::boundary::kMaxMessageBytes;
using tessera::ui::boundary::kRootBoundary;
using tessera::ui::boundary::make_error;
using tessera::ui::boundary::MemoryReportSink;
using tessera::ui::boundary::PlatformFailure;
using tessera::ui::boundary::test::RecordingOwner;
using tessera::ui::boundary::test::RecordingRoot;
using tessera::ui::boundary::test::RecordingSink;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "boundary_model: " << message << "\n";
    }
    return condition;
}

BoundaryConfig make_config(const char* component, const char* fallback = "subtree::fallback") {
    BoundaryConfig config;
    config.component = component;
    config.subtree = "subtree::child";
    config.fallback = fallback;
    return config;
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

    // --- registration -------------------------------------------------------
    const auto missing_fallback = table.add(make_config("card", ""));
    check(!missing_fallback.has_value() && missing_fallback.error().code == "fallback_required",
          "a boundary requires a fallback subtree");

    BoundaryConfig orphan = make_config("card");
    orphan.parent = 999;
    const auto missing_parent = table.add(orphan);
    check(!missing_parent.has_value() && missing_parent.error().code == "parent_unknown",
          "a parent boundary must be registered first");

    // --- a normal catch shows the fallback and reports once -----------------
    RecordingSink on_error;
    RecordingOwner owner;
    const auto card_result = table.add(make_config("checkout.card"), &on_error, &owner);
    check(card_result.has_value(), "a valid boundary registers");
    const BoundaryId card = card_result.value_or(kRootBoundary);

    const CatchOutcome caught =
        table.catch_error(card, make_error(std::runtime_error("boom")), FailureSite::Build, 0.0);
    check(caught.disposition == BoundaryDisposition::Fallback,
          "a recoverable error shows the fallback");
    check(caught.handled_by == card, "the owning boundary handles it");
    check(caught.reported, "every catch is reported");
    check(on_error.reports.size() == 1, "the catch is reported exactly once");
    check(on_error.last()->code == BoundaryCode::Caught, "the report carries CAUGHT");
    check(on_error.last()->component == "checkout.card", "the report names the component");
    check(on_error.last()->error.code == "exception" && on_error.last()->error.message == "boom",
          "the error value survives the catch");
    check(on_error.last()->site == FailureSite::Build, "the report records the failure site");
    check(on_error.last()->generation == 0 && on_error.last()->retries == 0,
          "a first catch reports generation and retry zero");
    check(root.reports.empty(), "a normal catch never reaches the root handler");
    check(owner.count("subtree::child") == 1, "the failed child is disposed exactly once");

    const BoundaryState* card_state = table.state(card);
    check(card_state != nullptr && card_state->phase == BoundaryPhase::Fallback,
          "the boundary moves to the fallback phase");
    check(card_state != nullptr && card_state->generation == 0,
          "a catch does not bump the generation");
    check(card_state != nullptr && card_state->child_detached && card_state->fallback_live,
          "child detached and fallback live");

    // A failure while the fallback is showing belongs to the parent (here: the root handler).
    const CatchOutcome again =
        table.catch_error(card, make_error(std::runtime_error("again")), FailureSite::Update, 0.0);
    check(again.disposition == BoundaryDisposition::Escalated && again.handled_by == kRootBoundary,
          "a boundary already in Fallback does not catch again");
    check(root.reports.size() == 1 && root.reports.front().boundary == kRootBoundary,
          "the root handler takes the propagated failure");
    check(on_error.reports.size() == 1, "no second onError callback for the same boundary");

    // --- catastrophic classes bypass boundaries -----------------------------
    RecordingSink fatal_sink;
    const auto fatal_result = table.add(make_config("fatal.card"), &fatal_sink);
    const BoundaryId fatal = fatal_result.value_or(kRootBoundary);
    const CatchOutcome oom =
        table.catch_error(fatal, make_error(std::bad_alloc{}), FailureSite::Build, 0.0);
    check(oom.disposition == BoundaryDisposition::Escalated, "OOM is not caught by a boundary");
    check(fatal_sink.reports.empty(), "a boundary never sees an OOM report");
    check(table.state(fatal) != nullptr && table.state(fatal)->phase == BoundaryPhase::Normal,
          "an OOM leaves the boundary untouched");

    const CatchOutcome platform =
        table.catch_error(fatal, make_error(PlatformFailure{}), FailureSite::Paint, 0.0);
    check(platform.disposition == BoundaryDisposition::Escalated, "device loss goes to the root");
    check(platform.error.kind == tessera::ui::boundary::ErrorClass::Platform,
          "a platform failure keeps its class");

    const std::size_t root_before_cancel = root.reports.size();
    const CatchOutcome cancelled =
        table.catch_error(card, make_error(CancellationSignal{}), FailureSite::Effect, 0.0);
    check(cancelled.disposition == BoundaryDisposition::Cancelled,
          "cancellation is a control outcome, not a fallback");
    check(!cancelled.reported, "cancellation is not reported as an error");
    check(root.reports.size() == root_before_cancel,
          "cancellation does not reach the root error handler");

    // --- catchEvents gates only event-handler throws ------------------------
    RecordingSink strict_sink;
    BoundaryConfig strict = make_config("strict.card");
    strict.catch_events = false;
    const auto strict_result = table.add(strict, &strict_sink);
    const BoundaryId strict_id = strict_result.value_or(kRootBoundary);
    const CatchOutcome event = table.catch_error(strict_id, make_error(std::runtime_error("click")),
                                                 FailureSite::Event, 0.0);
    check(event.disposition == BoundaryDisposition::Escalated && strict_sink.reports.empty(),
          "catchEvents=false sends an event throw to the root handler");
    const CatchOutcome strict_build = table.catch_error(
        strict_id, make_error(std::runtime_error("build")), FailureSite::Build, 0.0);
    check(strict_build.disposition == BoundaryDisposition::Fallback,
          "catchEvents=false still catches non-event throws");

    // --- reset --------------------------------------------------------------
    const std::size_t resets_before = diagnostics.count(BoundaryCode::Reset);
    check(table.reset(card), "reset clears a caught error");
    check(table.state(card) != nullptr && table.state(card)->phase == BoundaryPhase::Normal,
          "reset returns the boundary to normal");
    check(table.state(card) != nullptr && table.state(card)->generation == 1,
          "reset bumps the generation");
    check(table.state(card) != nullptr && table.state(card)->error.code.empty(),
          "reset clears the stored error");
    check(owner.count("subtree::fallback") == 1, "reset disposes the fallback subtree once");
    check(diagnostics.count(BoundaryCode::Reset) == resets_before + 1,
          "reset is reported to diagnostics");
    check(!table.reset(card), "resetting a healthy boundary is a no-op");
    check(diagnostics.count(BoundaryCode::Reset) == resets_before + 1,
          "a no-op reset is not reported");

    // --- no silent catch: a boundary without onError reports to diagnostics --
    const std::size_t caught_before = diagnostics.count(BoundaryCode::Caught);
    const auto quiet_result = table.add(make_config("quiet.card"));
    const BoundaryId quiet = quiet_result.value_or(kRootBoundary);
    const CatchOutcome quiet_catch =
        table.catch_error(quiet, make_error(std::runtime_error("quiet")), FailureSite::Effect, 0.0);
    check(quiet_catch.reported, "a boundary without onError still reports the catch");
    check(diagnostics.count(BoundaryCode::Caught) == caught_before + 1,
          "a boundary without onError still reports to diagnostics");

    // --- redaction and bounded messages ------------------------------------
    RecordingSink redaction_sink;
    const auto redaction_result = table.add(make_config("redact.card"), &redaction_sink);
    const BoundaryId redaction = redaction_result.value_or(kRootBoundary);
    const CatchOutcome redacted = table.catch_error(
        redaction, make_error(std::runtime_error("open C:\\Users\\alice\\notes.txt failed")),
        FailureSite::Build, 0.0);
    check(redacted.reported, "the redaction case is reported");
    check(redaction_sink.last() != nullptr &&
              redaction_sink.last()->error.message == "open <home>\\notes.txt failed",
          "home paths are redacted before the report leaves the boundary");
    const CatchOutcome bounded = table.catch_error(
        redaction, make_error(std::runtime_error(std::string(2000, 'x'))), FailureSite::Build, 0.0);
    check(bounded.reported, "the bounded-message case is reported");
    check(redaction_sink.last() != nullptr &&
              redaction_sink.last()->error.message.size() <= kMaxMessageBytes,
          "a long message is bounded");

    // --- unmount cleanup ----------------------------------------------------
    RecordingOwner remove_owner;
    const auto remove_result =
        table.add(make_config("remove.card", "subtree::remove.fallback"), nullptr, &remove_owner);
    const BoundaryId removable = remove_result.value_or(kRootBoundary);
    const CatchOutcome removable_catch = table.catch_error(
        removable, make_error(std::runtime_error("die")), FailureSite::Build, 0.0);
    check(removable_catch.disposition == BoundaryDisposition::Fallback,
          "the removable boundary catches first");
    check(remove_owner.count("subtree::child") == 1, "the move-to-fallback cleanup ran once");
    check(table.remove(removable) == 1, "unmount removes one boundary");
    check(remove_owner.count("subtree::child") == 1,
          "unmount does not dispose the already detached child again");
    check(remove_owner.count("subtree::remove.fallback") == 1,
          "unmount disposes the live fallback");
    check(!table.contains(removable), "an unmounted boundary is gone");
    const CatchOutcome orphaned = table.catch_error(
        removable, make_error(std::runtime_error("late")), FailureSite::Build, 0.0);
    check(orphaned.disposition == BoundaryDisposition::Escalated,
          "a removed boundary cannot catch; the root handler takes it");

    // --- registration cap ---------------------------------------------------
    check(diagnostics.dropped() == 0, "the diagnostics ring kept every report");

    if (failures != 0) {
        std::cerr << "boundary_model: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "boundary_model: ok\n";
    return 0;
}
