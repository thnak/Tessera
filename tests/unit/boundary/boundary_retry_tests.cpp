// SPDX-License-Identifier: MIT

// Decided auto-retry policy: pure deterministic state machine with injected fixed-step time.
// See docs/specs/error-boundaries-v1.md section 4.1.

#include "boundary_test_support.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using tessera::ui::boundary::backoff_seconds;
using tessera::ui::boundary::BoundaryCode;
using tessera::ui::boundary::BoundaryConfig;
using tessera::ui::boundary::BoundaryDisposition;
using tessera::ui::boundary::BoundaryId;
using tessera::ui::boundary::BoundaryPhase;
using tessera::ui::boundary::BoundaryTable;
using tessera::ui::boundary::CatchOutcome;
using tessera::ui::boundary::FailureSite;
using tessera::ui::boundary::kMaxAutoRetries;
using tessera::ui::boundary::kRootBoundary;
using tessera::ui::boundary::make_error;
using tessera::ui::boundary::MemoryReportSink;
using tessera::ui::boundary::RetryDecision;
using tessera::ui::boundary::RetryPolicy;
using tessera::ui::boundary::RetryState;
using tessera::ui::boundary::test::RecordingOwner;
using tessera::ui::boundary::test::RecordingRoot;
using tessera::ui::boundary::test::RecordingSink;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "boundary_retry: " << message << "\n";
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

    // --- the published schedule is exact ------------------------------------
    check(backoff_seconds(0) == 0.0, "retry ordinal 0 has no backoff");
    check(backoff_seconds(1) == 0.25, "retry 1 backs off 250 ms");
    check(backoff_seconds(2) == 0.5, "retry 2 backs off 500 ms");
    check(backoff_seconds(3) == 1.0, "retry 3 backs off 1000 ms");
    check(backoff_seconds(4) == 2.0, "retry 4 backs off 2000 ms");
    check(backoff_seconds(5) == 4.0, "retry 5 backs off 4000 ms");
    check(backoff_seconds(6) == 4.0, "the backoff is capped at 4000 ms");

    // --- auto-retry is off by default --------------------------------------
    RetryPolicy off;
    check(!off.enabled(), "auto-retry is off by default");
    const RetryDecision disabled = off.on_failure(0.0);
    check(disabled.state == RetryState::Disabled && !disabled.loop,
          "with auto-retry off a failure is disabled, not a loop");
    check(disabled.delay_seconds == 0.0 && disabled.deadline_seconds == 0.0,
          "auto-retry off schedules nothing");

    // --- the schedule doubles from the injected failure time ----------------
    RetryPolicy policy(kMaxAutoRetries);
    const double expected[5] = {0.25, 0.5, 1.0, 2.0, 4.0};
    double now = 100.0;
    for (std::size_t index = 0; index < 5; ++index) {
        const RetryDecision decision = policy.on_failure(now);
        check(decision.state == RetryState::Waiting, "a failure schedules a bounded retry");
        check(decision.retry == index + 1, "the retry ordinal advances");
        check(decision.delay_seconds == expected[index], "the backoff matches 250 ms doubling");
        check(decision.deadline_seconds == now + expected[index],
              "the deadline is injected time plus the backoff");
        now = decision.deadline_seconds;
    }

    // --- time is injected: the retry is ready exactly at the deadline -------
    RetryPolicy clocked(1);
    const RetryDecision wait = clocked.on_failure(10.0);
    check(wait.state == RetryState::Waiting && wait.deadline_seconds == 10.25,
          "the first failure schedules the retry deadline");
    check(clocked.advance(10.249).state == RetryState::Waiting,
          "a retry before its deadline is still waiting");
    const RetryDecision ready = clocked.advance(10.25);
    check(ready.state == RetryState::Ready, "a retry at its deadline is ready");
    check(ready.retry == 1 && ready.delay_seconds == 0.25, "the ready decision keeps the schedule");

    // --- the budget is capped, then the boundary stops retrying --------------
    RetryPolicy capped(9); // clamped to kMaxAutoRetries
    check(capped.max_auto_retries() == kMaxAutoRetries,
          "maxAutoRetries is clamped to the policy cap");
    std::vector<double> delays;
    bool looped = false;
    for (std::size_t attempt = 0; attempt < 8; ++attempt) {
        const RetryDecision decision = capped.on_failure(0.0);
        if (decision.state == RetryState::Disabled) {
            looped = decision.loop;
            break;
        }
        delays.push_back(decision.delay_seconds);
    }
    check(delays.size() == 5, "at most five retries are scheduled");
    check(looped, "exhausting the budget reports the loop guard");
    check(RetryPolicy(0).used() == 0 && RetryPolicy(0).on_failure(0.0).loop == false,
          "an unconfigured policy never reports a loop");

    // --- reset restores the full budget -------------------------------------
    capped.reset();
    check(!capped.loop() && capped.used() == 0, "reset clears the budget and the loop flag");
    check(capped.on_failure(0.0).delay_seconds == 0.25, "reset restarts at 250 ms");

    // --- the boundary table applies the same policy -------------------------
    RecordingRoot root;
    MemoryReportSink diagnostics;
    BoundaryTable table(root, diagnostics);

    RecordingSink on_error;
    RecordingOwner owner;
    BoundaryConfig config;
    config.component = "retry.card";
    config.subtree = "subtree::retry";
    config.fallback = "subtree::retry.fallback";
    config.max_auto_retries = 2;
    const auto added = table.add(config, &on_error, &owner);
    check(added.has_value(), "the retry boundary registers");
    const BoundaryId id = added.value_or(kRootBoundary);

    // Attempt 1 fails at t=0 and schedules the first retry.
    const CatchOutcome first =
        table.catch_error(id, make_error(std::runtime_error("one")), FailureSite::Update, 0.0);
    check(first.retry.state == RetryState::Waiting && first.retry.delay_seconds == 0.25,
          "the first failure schedules a 250 ms retry");
    check(table.advance(id, 0.0).state == RetryState::Waiting,
          "the retry pump waits for the injected clock");
    const RetryDecision first_ready = table.advance(id, 0.25);
    check(first_ready.state == RetryState::Ready, "the first retry runs at exactly 250 ms");
    check(table.state(id) != nullptr && table.state(id)->phase == BoundaryPhase::Normal,
          "a ready retry reopens the boundary for the rebuilt child");
    check(table.state(id) != nullptr && table.state(id)->generation == 1,
          "a ready retry bumps the generation");
    check(owner.count("subtree::retry.fallback") == 1, "a ready retry disposes the fallback");

    // Attempt 2 fails at t=0.25 and doubles the backoff.
    const CatchOutcome second =
        table.catch_error(id, make_error(std::runtime_error("two")), FailureSite::Update, 0.25);
    check(second.retry.state == RetryState::Waiting && second.retry.delay_seconds == 0.5,
          "the second failure doubles the backoff to 500 ms");
    check(second.retry.deadline_seconds == 0.75, "the second deadline is 0.75 s");
    check(table.advance(id, 0.75).state == RetryState::Ready,
          "the second retry runs at exactly 750 ms");

    // Attempt 3 exhausts maxAutoRetries == 2: the loop guard takes over.
    const CatchOutcome third =
        table.catch_error(id, make_error(std::runtime_error("three")), FailureSite::Update, 0.75);
    check(third.retry.state == RetryState::Disabled && third.retry.loop,
          "exhausting maxAutoRetries disables retry for the boundary");
    check(third.disposition == BoundaryDisposition::Fallback,
          "the fallback stays shown after the budget is spent");
    check(on_error.count(BoundaryCode::Loop) == 1, "the loop guard is reported once");
    check(table.state(id) != nullptr && table.state(id)->loop_reported,
          "the boundary records that the loop guard fired");

    // The child stays detached while the fallback is shown, so a later failure against the same
    // boundary escalates instead of re-entering the catch loop.
    const CatchOutcome fourth =
        table.catch_error(id, make_error(std::runtime_error("four")), FailureSite::Update, 1.0);
    check(fourth.disposition == BoundaryDisposition::Escalated,
          "a failure while the fallback is shown escalates to the parent");
    check(on_error.count(BoundaryCode::Loop) == 1, "the loop guard does not spam reports");
    check(table.advance(id, 1000.0).state == RetryState::Disabled,
          "the retry pump stays disabled; nothing spins");

    // A manual reset clears the loop guard and restores the budget.
    check(table.reset(id), "a manual reset clears the failure");
    check(table.state(id) != nullptr && !table.state(id)->loop_reported,
          "reset clears the loop flag");
    const CatchOutcome after_reset =
        table.catch_error(id, make_error(std::runtime_error("five")), FailureSite::Update, 2000.0);
    check(after_reset.retry.state == RetryState::Waiting && after_reset.retry.delay_seconds == 0.25,
          "a manual reset restarts the retry schedule");

    if (failures != 0) {
        std::cerr << "boundary_retry: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "boundary_retry: ok\n";
    return 0;
}
