// SPDX-License-Identifier: MIT

#include "tessera/ui/boundary/boundary.hpp"

#include <utility>

namespace tessera::ui::boundary {
namespace {

[[nodiscard]] bool is_catastrophic(ErrorClass kind) {
    return kind == ErrorClass::Fatal || kind == ErrorClass::Platform;
}

// Async completions are produced by effect-owned tasks, so an async report carries the effect site
// instead of inventing a site the generated code never has (section 6).
constexpr FailureSite kAsyncSite = FailureSite::Effect;

} // namespace

CatchOutcome BoundaryTable::catch_error(BoundaryId from, Error error, FailureSite site,
                                        double now_seconds) {
    Error value = bounded_error(std::move(error));

    if (value.kind == ErrorClass::Cancellation) {
        // Cancellation is a control outcome, not an error (sections 1 and 6): it never shows a
        // fallback, never calls onError, never touches retries, and leaves the boundary state
        // alone. It must still never be invisible: a cancellation signal caught as an exception at
        // the boundary produces exactly one non-error notification to the root handler, and the
        // table counts it for diagnostics (AGENTS.md non-negotiable 16). The async value path stays
        // silent because the task owner produced that value (section 6).
        std::uint32_t retries = 0;
        std::uint64_t generation = 0;
        if (const Entry* entry = find(from); entry != nullptr) {
            retries = entry->state.retries;
            generation = entry->state.generation;
        }
        Report report = make_report(BoundaryCode::Cancelled, kRootBoundary, component_of(from),
                                    value, site, retries, generation);
        ++cancellation_notifications_;
        escalate(report);

        CatchOutcome outcome;
        outcome.disposition = BoundaryDisposition::Cancelled;
        outcome.handled_by = from;
        outcome.error = report.error;
        return outcome;
    }

    if (is_catastrophic(value.kind)) {
        // OOM/bad_alloc, abort, and platform/device loss bypass every component boundary.
        return escalate_error(from, std::move(value), site, BoundaryCode::Caught);
    }

    const BoundaryId target = catcher(from);
    if (target == kRootBoundary) {
        return escalate_error(from, std::move(value), site, BoundaryCode::Caught);
    }

    Entry* entry = find(target);
    if (entry == nullptr) {
        return escalate_error(from, std::move(value), site, BoundaryCode::Caught);
    }
    if (is_event_site(site) && !entry->config.catch_events) {
        // catchEvents == false is the strict-debugging switch: the event throw reaches the root
        // handler instead of a fallback, and still never escapes into a thread/FFI edge.
        return escalate_error(target, std::move(value), site, BoundaryCode::Caught);
    }
    return show_fallback(*entry, std::move(value), site, BoundaryCode::Caught, now_seconds, true);
}

CatchOutcome BoundaryTable::fallback_failed(BoundaryId id, Error error, double now_seconds) {
    Entry* entry = find(id);
    if (entry == nullptr) {
        return escalate_error(id, std::move(error), kAsyncSite, BoundaryCode::FallbackFailed);
    }

    // The fallback is the failed subtree: dispose it, keep the boundary in Fallback with nothing
    // live, and hand the failure to the parent boundary/root (section 4).
    if (entry->state.fallback_live) {
        if (entry->owner != nullptr) {
            entry->owner->dispose_subtree(entry->config.fallback);
        }
        entry->state.fallback_live = false;
    }
    entry->state.error = bounded_error(std::move(error));
    // A fallback failure never auto-retries: re-running the fallback that just failed is a loop.
    entry->policy.reset();
    entry->state.retries = 0;

    Error value = entry->state.error;
    const BoundaryId target = catcher(entry->config.parent);
    if (target == kRootBoundary) {
        return escalate_error(id, std::move(value), kAsyncSite, BoundaryCode::FallbackFailed);
    }
    Entry* parent = find(target);
    if (parent == nullptr) {
        return escalate_error(id, std::move(value), kAsyncSite, BoundaryCode::FallbackFailed);
    }
    return show_fallback(*parent, std::move(value), kAsyncSite, BoundaryCode::FallbackFailed,
                         now_seconds, false);
}

CatchOutcome BoundaryTable::report_async(BoundaryId owner, std::uint64_t generation, Error error,
                                         double now_seconds) {
    Error value = bounded_error(std::move(error));

    if (value.kind == ErrorClass::Cancellation) {
        // Cancellation of an async task is not an error: no fallback, no report (section 6).
        CatchOutcome outcome;
        outcome.disposition = BoundaryDisposition::Cancelled;
        outcome.handled_by = owner;
        outcome.error = std::move(value);
        return outcome;
    }

    const Entry* entry = find(owner);
    if (entry == nullptr || generation != entry->state.generation) {
        // The boundary or the child generation is gone: drop the report with a diagnostic and never
        // call onError for it, so a stale task cannot resurrect a dead subtree (section 6).
        CatchOutcome outcome;
        Report report =
            make_report(BoundaryCode::Async, owner, component_of(owner), std::move(value),
                        kAsyncSite, entry != nullptr ? entry->state.retries : 0, generation);
        report.dropped = true;
        outcome.disposition = BoundaryDisposition::Dropped;
        outcome.handled_by = owner;
        outcome.dropped = true;
        outcome.reported = true;
        outcome.error = report.error;
        diagnostics_.on_report(report);
        return outcome;
    }

    if (is_catastrophic(value.kind)) {
        return escalate_error(owner, std::move(value), kAsyncSite, BoundaryCode::Async);
    }

    const BoundaryId target = catcher(owner);
    if (target == kRootBoundary) {
        return escalate_error(owner, std::move(value), kAsyncSite, BoundaryCode::Async);
    }
    Entry* catcher_entry = find(target);
    if (catcher_entry == nullptr) {
        return escalate_error(owner, std::move(value), kAsyncSite, BoundaryCode::Async);
    }
    // Async values show the fallback but never consume the auto-retry budget: auto-retry re-runs
    // generated component code, and there is no inline async re-run to schedule.
    return show_fallback(*catcher_entry, std::move(value), kAsyncSite, BoundaryCode::Async,
                         now_seconds, false);
}

RetryDecision BoundaryTable::advance(BoundaryId id, double now_seconds) {
    Entry* entry = find(id);
    if (entry == nullptr) {
        return {};
    }
    const RetryDecision decision = entry->policy.advance(now_seconds);
    if (decision.state != RetryState::Ready) {
        return decision;
    }

    // The retry rebuilds the child: dispose the fallback, reopen the boundary, and bump the
    // generation so late async completions from the failed child are dropped.
    if (entry->state.fallback_live) {
        if (entry->owner != nullptr) {
            entry->owner->dispose_subtree(entry->config.fallback);
        }
        entry->state.fallback_live = false;
    }
    entry->state.phase = BoundaryPhase::Normal;
    entry->state.child_detached = false;
    entry->state.error = {};
    entry->state.generation += 1;
    return decision;
}

bool BoundaryTable::reset(BoundaryId id) {
    Entry* entry = find(id);
    if (entry == nullptr) {
        return false;
    }
    const bool clean = entry->state.phase == BoundaryPhase::Normal && !entry->state.fallback_live &&
                       !entry->state.child_detached && entry->state.error.code.empty();
    if (clean) {
        // A resetKey change on a healthy boundary is not an event; no RESET report is emitted.
        return false;
    }

    // Reset disposes the fallback and reopens the boundary; the caller rebuilds the child subtree.
    if (entry->state.fallback_live) {
        if (entry->owner != nullptr) {
            entry->owner->dispose_subtree(entry->config.fallback);
        }
        entry->state.fallback_live = false;
    }
    entry->state.phase = BoundaryPhase::Normal;
    entry->state.child_detached = false;
    entry->state.error = {};
    entry->state.retries = 0;
    entry->state.loop_reported = false;
    entry->state.generation += 1;
    entry->policy.reset();

    // A reset is not an error, so its report goes to diagnostics only (section 7 code list).
    diagnostics_.on_report(make_report(BoundaryCode::Reset, id, entry->config.component, Error{},
                                       FailureSite::Build, 0, entry->state.generation));
    return true;
}

CatchOutcome BoundaryTable::escalate_error(BoundaryId source, Error error, FailureSite site,
                                           BoundaryCode code) {
    std::uint32_t retries = 0;
    std::uint64_t generation = 0;
    if (const Entry* entry = find(source); entry != nullptr) {
        retries = entry->state.retries;
        generation = entry->state.generation;
    }

    CatchOutcome outcome;
    Report report = make_report(code, kRootBoundary, component_of(source), std::move(error), site,
                                retries, generation);
    outcome.error = report.error;
    outcome.disposition = BoundaryDisposition::Escalated;
    outcome.handled_by = kRootBoundary;
    outcome.reported = true;
    escalate(report);
    return outcome;
}

CatchOutcome BoundaryTable::show_fallback(Entry& entry, Error error, FailureSite site,
                                          BoundaryCode code, double now_seconds, bool allow_retry) {
    // Freeze/detach the failed child exactly once, through the single cleanup seam.
    if (!entry.state.child_detached) {
        if (entry.owner != nullptr) {
            entry.owner->dispose_subtree(entry.config.subtree);
        }
        entry.state.child_detached = true;
    }
    entry.state.phase = BoundaryPhase::Fallback;
    entry.state.fallback_live = true;

    CatchOutcome outcome;
    Report report = make_report(code, entry.id, entry.config.component, std::move(error), site,
                                entry.state.retries, entry.state.generation);
    entry.state.error = report.error;
    outcome.error = report.error;
    outcome.disposition = BoundaryDisposition::Fallback;
    outcome.handled_by = entry.id;
    outcome.reported = true;
    // One catch, one report: the onError channel (or the diagnostics sink) is always called.
    deliver(entry, report);

    if (!allow_retry) {
        outcome.retry = entry.policy.pending();
        return outcome;
    }

    const RetryDecision decision = entry.policy.on_failure(now_seconds);
    entry.state.retries = entry.policy.used();
    outcome.retry = decision;
    if (decision.loop && !entry.state.loop_reported) {
        // The retry budget is spent: loop protection disables auto-retry for this boundary and
        // reports TESSERA_BOUNDARY_LOOP once per failure episode.
        entry.state.loop_reported = true;
        deliver(entry,
                make_report(BoundaryCode::Loop, entry.id, entry.config.component, entry.state.error,
                            site, entry.state.retries, entry.state.generation));
    }
    return outcome;
}

} // namespace tessera::ui::boundary
