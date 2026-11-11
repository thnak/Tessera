// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/diagnostics/log.hpp"
#include "tessera/ui/layout/layout_observer.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::ui::layout {

// Consecutive ticks an observation backlog may persist before TESSERA_LAYOUT_OBSERVER_LOOP is
// reported (docs/design/layout-observation.md section 5.3).
inline constexpr std::uint32_t kLoopReportTicks = 8;

// Valid range of the runtime-config key `layoutObservationMaxRounds`
// (docs/specs/runtime-config-v1.md).
inline constexpr std::uint32_t kMinObservationRounds = 1;
inline constexpr std::uint32_t kMaxObservationRounds = 16;
inline constexpr std::uint32_t kDefaultObservationRounds = 4;

// Clamps a configured round budget into the documented range.
[[nodiscard]] std::uint32_t clamp_observation_rounds(std::int64_t configured);

// Counters of one `observe_and_settle` call.
struct SettleStats {
    std::uint32_t rounds = 0;     // collect/deliver rounds that delivered something
    std::uint32_t deliveries = 0; // callbacks invoked
    std::uint32_t relayouts = 0;  // times the host relayout callback ran
    std::size_t deferred = 0;     // observations still due when the call returned
    bool round_limit_hit = false; // the budget ended the loop while work remained
    bool loop_reported = false;   // TESSERA_LAYOUT_OBSERVER_LOOP was logged by this call
};

// Running totals (the frame-diagnostics counters of section 5.3).
struct SettleTotals {
    std::uint64_t deliveries = 0;
    std::uint64_t rounds = 0;
    std::uint64_t deferred = 0;
    std::uint64_t round_limit_hits = 0;
    std::uint64_t loop_reports = 0;
};

// Host-provided hooks. `relayout` must run style, layout, `apply_layout` and
// `LayoutReadModel::publish` for the state changes the callbacks scheduled; `wake` asks the
// runtime loop for another frame (typically `RuntimeLoop::request_frame`). `logger` may be null.
struct SettleHooks {
    using RelayoutFn = void (*)(void* context);
    using WakeFn = void (*)(void* context);

    RelayoutFn relayout = nullptr;
    void* relayout_context = nullptr;
    WakeFn wake = nullptr;
    void* wake_context = nullptr;
    diagnostics::Logger* logger = nullptr;
};

class LayoutSettleState;

SettleStats observe_and_settle(LayoutObserverSet& observers, const LayoutReadModel& model,
                               UpdateScheduler& scheduler, const SettleHooks& hooks,
                               LayoutSettleState& state);

// Persistent per-window state of the settle loop: the totals and the consecutive-tick counter that
// drives the oscillation diagnostic.
class LayoutSettleState {
public:
    [[nodiscard]] const SettleTotals& totals() const {
        return totals_;
    }
    [[nodiscard]] std::uint32_t backlog_ticks() const {
        return backlog_ticks_;
    }

private:
    friend SettleStats observe_and_settle(LayoutObserverSet&, const LayoutReadModel&,
                                          UpdateScheduler&, const SettleHooks&, LayoutSettleState&);
    SettleTotals totals_;
    std::uint32_t backlog_ticks_ = 0;
};

// The observation step of one tick (section 5.1, steps 4b). Call it right after the first layout
// of the tick was applied and published. Round 1 delivers every changed observation. If the
// callbacks scheduled updates, the scheduler is flushed and `relayout` runs, and the next round
// delivers only observations strictly deeper than the shallowest one delivered before (the depth
// rule), until nothing is due, nothing changed state, or `observers.max_rounds()` is reached.
// Whatever is left stays due for the next tick and wakes the loop; a backlog that lasts
// `kLoopReportTicks` ticks is logged once as TESSERA_LAYOUT_OBSERVER_LOOP.
//
// With nothing attached this costs one size check. UI thread only.
SettleStats observe_and_settle(LayoutObserverSet& observers, const LayoutReadModel& model,
                               UpdateScheduler& scheduler, const SettleHooks& hooks,
                               LayoutSettleState& state);

} // namespace tessera::ui::layout
