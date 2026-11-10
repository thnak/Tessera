// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/boundary/report.hpp"
#include "tessera/ui/boundary/retry_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::boundary {

// Live boundaries per table. Registration is a component-tree operation, not a per-frame one, so a
// fixed cap keeps the model allocation-bounded and a full table is a recoverable error value.
inline constexpr std::size_t kMaxBoundaries = 64;

struct BoundaryConfig {
    BoundaryId parent = kRootBoundary; // kRootBoundary attaches the boundary to the root handler
    std::string component;             // diagnostics id (never user content)
    std::string subtree;               // owned child subtree id
    std::string fallback;              // fallback subtree id; required
    std::uint32_t max_auto_retries = 0;
    bool catch_events = true;
};

// Lifecycle of one boundary (section 4). `Fallback` means the failed child was detached and the
// fallback subtree is live; a boundary in `Fallback` no longer catches for its child, so a failure
// inside the fallback propagates to the parent.
enum class BoundaryPhase : std::uint8_t { Normal, Fallback };

struct BoundaryState {
    BoundaryPhase phase = BoundaryPhase::Normal;
    Error error;                  // last caught error, already redacted
    std::uint32_t retries = 0;    // retries spent on the current failure episode
    std::uint64_t generation = 0; // bumped on every reset; stale async reports compare it
    bool loop_reported = false;   // TESSERA_BOUNDARY_LOOP was reported for this episode
    bool fallback_live = false;
    bool child_detached = false;
};

// Subtree ownership seam. The runtime that owns scene nodes, tasks, and animations implements this
// so a catch, a reset, or an unmount cancels child work and releases refs exactly once (section 8).
class SubtreeOwner {
public:
    virtual ~SubtreeOwner() = default;

    // Must not throw: the boundary is already handling a failure when this runs.
    virtual void dispose_subtree(std::string_view subtree) = 0;
};

// Where a failure ended up: the boundary showed its fallback, the parent/root took it, it was a
// cancellation (a control outcome, not an error), or a stale async value was dropped.
enum class BoundaryDisposition : std::uint8_t { Fallback, Escalated, Cancelled, Dropped };

struct CatchOutcome {
    BoundaryDisposition disposition = BoundaryDisposition::Fallback;
    BoundaryId handled_by = kRootBoundary; // kRootBoundary = the root handler took it
    RetryDecision retry;                   // pending auto-retry; `retry.state` says which
    Error error;                           // value copy: no exception escapes the boundary
    // True when an error report reached onError/diagnostics. A cancellation is not an error
    // report: it is notified to the root handler and counted instead (section 11, rule 4).
    bool reported = false;
    bool dropped = false; // stale async value: diagnostics only, no callback
};

// Last catch point (section 1): catastrophic classes, fallback failures, and boundaries with
// `catch_events == false` land here instead of in a component boundary.
class RootHandler {
public:
    virtual ~RootHandler() = default;

    // Must not throw and must not re-enter the table.
    virtual void on_root_error(const Report& report) = 0;
};

// The boundary model: a registry of subtrees, their fallback/onError ownership, and the routing
// rule that decides between the nearest boundary and the root handler.
class BoundaryTable {
public:
    // Both collaborators outlive the table. `diagnostics` receives reports from boundaries without
    // onError, resets, dropped async values, and retry-loop reports.
    BoundaryTable(RootHandler& root, ReportSink& diagnostics);

    BoundaryTable(const BoundaryTable&) = delete;
    BoundaryTable& operator=(const BoundaryTable&) = delete;

    // Register a boundary. Returns the new id, or a recoverable Error when the table is full, the
    // fallback is missing, or the parent is not registered.
    [[nodiscard]] std::expected<BoundaryId, Error>
    add(BoundaryConfig config, ReportSink* on_error = nullptr, SubtreeOwner* owner = nullptr);

    // Unmount: disposes the boundary and every descendant, drops pending retries, and never calls
    // onError afterwards. Returns how many boundaries were removed.
    [[nodiscard]] std::size_t remove(BoundaryId id);

    [[nodiscard]] bool contains(BoundaryId id) const;
    // Pointers are invalidated by add/remove, which is why ids (not pointers) are the public
    // handle.
    [[nodiscard]] const BoundaryConfig* config(BoundaryId id) const;
    [[nodiscard]] const BoundaryState* state(BoundaryId id) const;

    // Monotonic diagnostics counter of cancellation notifications delivered to the root handler
    // (one per exception-path cancellation, never for an async cancellation value). Cancellation
    // is not an error, so it is counted here instead of being reported to onError.
    [[nodiscard]] std::uint64_t cancellation_notifications() const {
        return cancellation_notifications_;
    }

    // The boundary that would catch a failure raised in `from`'s subtree: the nearest boundary in
    // the Normal phase, or kRootBoundary when there is none. Also used by the guard for tests.
    [[nodiscard]] BoundaryId catcher(BoundaryId from) const;

    void set_on_error(BoundaryId id, ReportSink* sink);
    void set_owner(BoundaryId id, SubtreeOwner* owner);

    // The sync catch point's routing half: converts one failure into fallback selection or root
    // escalation, and reports exactly once. `now_seconds` is injected fixed-step time.
    //
    // A `Cancellation` value is the exception-path cancellation signal: no fallback, no state
    // change, no retry, no onError, but exactly one non-error `TESSERA_BOUNDARY_CANCELLED`
    // notification to the root handler, so a cancellation can never be invisible.
    [[nodiscard]] CatchOutcome catch_error(BoundaryId from, Error error, FailureSite site,
                                           double now_seconds);

    // The fallback (or onError) of `id` failed: report FALLBACK_FAILED to the parent boundary or
    // the root handler. Auto-retry is never applied to a fallback failure (that is a loop).
    [[nodiscard]] CatchOutcome fallback_failed(BoundaryId id, Error error, double now_seconds);

    // Async errors are values (section 6). Delivered on the UI thread at a frame boundary; a stale
    // id or generation is dropped with a diagnostic and never calls onError.
    [[nodiscard]] CatchOutcome report_async(BoundaryId owner, std::uint64_t generation, Error error,
                                            double now_seconds);

    // Fixed-step retry pump: returns Ready once the injected time reaches the deadline, rebuilds
    // the child subtree, and bumps the generation.
    [[nodiscard]] RetryDecision advance(BoundaryId id, double now_seconds);

    // Clears the error, disposes the fallback, bumps the generation, and resets the retry budget.
    // Returns false when the boundary is unknown or already clean (a resetKey change on a healthy
    // boundary must not emit a RESET report).
    bool reset(BoundaryId id);

private:
    struct Entry {
        BoundaryId id = kRootBoundary;
        BoundaryConfig config;
        BoundaryState state;
        RetryPolicy policy;
        ReportSink* on_error = nullptr;
        SubtreeOwner* owner = nullptr;
    };

    [[nodiscard]] Entry* find(BoundaryId id);
    [[nodiscard]] const Entry* find(BoundaryId id) const;

    // Delivers one report to the boundary's onError sink or the diagnostics sink. A sink that
    // throws violates the onError contract; that becomes a FALLBACK_FAILED report at the root
    // instead of an exception escaping the catch point.
    void deliver(Entry& entry, const Report& report);
    void escalate(const Report& report);
    [[nodiscard]] std::string component_of(BoundaryId id) const;

    // Root-side catch point for a failure that must not stay in a component boundary: catastrophic
    // classes, no registered catcher, and boundaries that opt out of catching events.
    [[nodiscard]] CatchOutcome escalate_error(BoundaryId source, Error error, FailureSite site,
                                              BoundaryCode code);

    // Shared by the sync and async paths: detach the child, show the fallback, report once, and
    // schedule an auto-retry when the policy allows one.
    [[nodiscard]] CatchOutcome show_fallback(Entry& entry, Error error, FailureSite site,
                                             BoundaryCode code, double now_seconds,
                                             bool allow_retry);

    RootHandler& root_;
    ReportSink& diagnostics_;
    std::vector<Entry> entries_;
    BoundaryId next_id_ = 1;
    std::uint64_t cancellation_notifications_ = 0;
};

} // namespace tessera::ui::boundary
