// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_observer_settle.hpp"

#include <string>

namespace tessera::ui::layout {

std::uint32_t clamp_observation_rounds(std::int64_t configured) {
    if (configured < static_cast<std::int64_t>(kMinObservationRounds)) {
        return kMinObservationRounds;
    }
    if (configured > static_cast<std::int64_t>(kMaxObservationRounds)) {
        return kMaxObservationRounds;
    }
    return static_cast<std::uint32_t>(configured);
}

SettleStats observe_and_settle(LayoutObserverSet& observers, const LayoutReadModel& model,
                               UpdateScheduler& scheduler, const SettleHooks& hooks,
                               LayoutSettleState& state) {
    SettleStats stats;
    if (observers.size() == 0 && hooks.engine_observe == nullptr) {
        state.backlog_ticks_ = 0;
        return stats;
    }

    const std::uint32_t max_rounds = observers.max_rounds();
    std::size_t depth_floor = 0;
    for (;;) {
        // Engine observers first, so app callbacks see sizes that already reflect container styles.
        if (hooks.engine_observe != nullptr &&
            hooks.engine_observe(hooks.engine_observe_context, model)) {
            ++stats.engine_rounds;
            if (stats.rounds + stats.engine_rounds >= max_rounds) {
                stats.round_limit_hit = true; // the styles it dirtied wait for the next tick
                break;
            }
            scheduler.flush();
            if (hooks.relayout != nullptr) {
                hooks.relayout(hooks.relayout_context);
                ++stats.relayouts;
            }
            continue;
        }
        if (observers.collect(model, depth_floor) == 0) {
            break;
        }
        const LayoutDeliveryStats delivered = observers.deliver();
        ++stats.rounds;
        stats.deliveries += delivered.delivered;
        if (!scheduler.pending()) {
            break; // the callbacks changed no state, so there is nothing to lay out again
        }
        if (stats.rounds + stats.engine_rounds >= max_rounds) {
            stats.round_limit_hit = true;
            break;
        }
        scheduler.flush();
        if (hooks.relayout != nullptr) {
            hooks.relayout(hooks.relayout_context);
            ++stats.relayouts;
        }
        // Depth rule: only nodes strictly deeper than the shallowest one just served may react.
        depth_floor = delivered.shallowest_depth + 1;
    }

    stats.deferred = observers.deferred();
    const bool backlog = stats.round_limit_hit || stats.deferred > 0;

    state.totals_.deliveries += stats.deliveries;
    state.totals_.rounds += stats.rounds;
    state.totals_.engine_rounds += stats.engine_rounds;
    state.totals_.deferred += stats.deferred;
    if (stats.round_limit_hit) {
        ++state.totals_.round_limit_hits;
    }

    if (!backlog) {
        state.backlog_ticks_ = 0;
        return stats;
    }

    // Correctness wins over idling: a stale size must not stay on screen, so ask for a frame.
    if (hooks.wake != nullptr) {
        hooks.wake(hooks.wake_context);
    }
    ++state.backlog_ticks_;
    if (state.backlog_ticks_ == kLoopReportTicks) {
        ++state.totals_.loop_reports;
        stats.loop_reported = true;
        if (hooks.logger != nullptr) {
            hooks.logger->log(diagnostics::LogLevel::Warn, diagnostics::LogCategory::Layout,
                              "TESSERA_LAYOUT_OBSERVER_LOOP: layout observations were still due "
                              "after " +
                                  std::to_string(kLoopReportTicks) +
                                  " consecutive ticks; a size callback probably changes the size "
                                  "it observes");
        }
    }
    return stats;
}

} // namespace tessera::ui::layout
