// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/diagnostics/log.hpp"
#include "tessera/ui/layout/container_query_tracker.hpp"
#include "tessera/ui/layout/layout_observer.hpp"
#include "tessera/ui/layout/layout_observer_settle.hpp"
#include "tessera/ui/layout/layout_read_model.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::ui::layout {

// What one `LayoutSettle::run` did, in the terms a host reports: the settle counters plus the two
// facts a caller acts on (whether a frame is owed, and whether the cascade gate was open at all).
struct LayoutSettleResult {
    SettleStats stats;
    // True when the stylesheet has `@container` rules AND at least one container is registered, so
    // the loop ran. False means the call cost one boolean test and nothing else.
    bool active = false;
    // True when the loop left work due (a round-limit hit or a deferred observation): the caller
    // must ask the runtime loop for another frame.
    bool deferred = false;
};

// The one implementation of ADR-041's layout settle loop that every host shares (issue #693): the
// read model, the engine-owned container observer and the app observations, driven by
// `observe_and_settle`. `AppHost` and the Studio canvas session both call it, so the preview cannot
// drift from the runtime.
//
// **Why a component and not a function.** The three pieces have to be created, gated, re-registered
// and torn down together, and the gate (`has_container_queries()`) has to be re-evaluated on every
// style rebuild because a theme or an embedded sheet can add `@container` later. Keeping them in
// one object is what makes that a single call rather than a protocol each host re-implements.
//
// **Cost when nothing observes.** `run` returns after one `has_container_queries()` test and one
// `size()` check: no read-model publish, no tracker, no loop. A stylesheet without `@container` and
// a scene without `onSizeChange` therefore pay nothing, which is the "no added work" criterion.
//
// UI thread only, like everything it wraps.
class LayoutSettle {
public:
    // `relayout` must do everything a host's restyle-and-layout step does (rebuild the selector
    // tree, drop the paint cache, run style and layout, apply it, publish the read model); a hook
    // that only re-lays-out would paint a stale style generation. `wake` asks the runtime loop for
    // another frame. Both may be null in a headless caller that drives the loop itself.
    using RelayoutFn = void (*)(void* context);
    using WakeFn = void (*)(void* context);

    LayoutSettle() = default;
    LayoutSettle(const LayoutSettle&) = delete;
    LayoutSettle& operator=(const LayoutSettle&) = delete;

    // The round budget (`layoutObservationMaxRounds`, 1..16). Out-of-range values clamp.
    void set_max_rounds(std::uint32_t rounds);
    [[nodiscard]] std::uint32_t max_rounds() const {
        return observers_.max_rounds();
    }

    void set_relayout(RelayoutFn relayout, void* context);
    void set_wake(WakeFn wake, void* context);
    // The structured logger the oscillation diagnostic goes to; null (the default) drops it.
    void set_logger(diagnostics::Logger* logger);

    // The window's update scheduler. Null is allowed and means "no app callbacks to flush", which
    // is the Studio canvas's case: it has no `UpdateScheduler`, and a dummy one would be a lie.
    void set_scheduler(UpdateScheduler* scheduler);

    // The app observations of this window (generated components attach to it). Never null: a host
    // that has no observing component simply leaves it empty, and `run` then costs one size check.
    [[nodiscard]] LayoutObserverSet& observers() {
        return observers_;
    }

    // Publishes one finished layout. Call it right after `apply_layout` wrote the boxes, before
    // `run`. `dpi_scale` is the window's device scale for this layout.
    void publish(const LayoutResult& result, float dpi_scale);

    // Points `tree`'s `@container` resolution at this loop's published layout. Call it on every
    // style tree, before the style pass: a host that rebuilds the tree per pass (the Studio canvas
    // session does) must re-install it, and a host that keeps one tree across frames installs it
    // once. A no-op while the gate is closed, so a sheet without `@container` never pays for it.
    void install_container_source(style::SceneStyleTree& tree);

    // Re-registers the query containers from the style pass (issue #693 decision 2). Call it after
    // every style pass. `tree_rebuilt` says whether the selector tree was rebuilt (the scene's
    // shape may have changed, so the tracker's `Node*` are stale and it is cleared) or only
    // restyled in place (the entries stay, so a container keeps the size it was last styled at and
    // a resize inside one condition still costs no engine round). `sheet` decides whether the gate
    // is open at all, and is re-tested here so a theme that adds `@container` opens it later.
    void register_containers(const Node& root, style::SceneStyleTree& tree,
                             const style::CompiledStyleSheet& sheet, bool tree_rebuilt);
    // One tick's settle step: the engine observer first, then the app observations, in bounded
    // rounds. Call it right after the tick's first layout was applied and published.
    LayoutSettleResult run();

    [[nodiscard]] const LayoutReadModel& read_model() const {
        return model_;
    }
    [[nodiscard]] const ContainerQueryTracker& tracker() const {
        return tracker_;
    }
    [[nodiscard]] const SettleTotals& totals() const {
        return state_.totals();
    }
    // True when the stylesheet has `@container` rules and at least one container is registered.
    [[nodiscard]] bool active() const {
        return gate_open_ && tracker_.size() != 0;
    }

private:
    static void relayout_thunk(void* context);
    static void wake_thunk(void* context);
    static bool engine_observe_thunk(void* context, const LayoutReadModel& model);

    LayoutReadModel model_;
    ContainerQueryTracker tracker_;
    // The `@container` conditions of the active sheet, kept because the tracker re-evaluates them
    // against each container's published size on every round.
    std::vector<style::ContainerQuery> queries_;
    LayoutObserverSet observers_{kDefaultObservationRounds};
    LayoutSettleState state_;
    UpdateScheduler* scheduler_ = nullptr;
    RelayoutFn relayout_ = nullptr;
    void* relayout_context_ = nullptr;
    WakeFn wake_ = nullptr;
    void* wake_context_ = nullptr;
    diagnostics::Logger* logger_ = nullptr;
    // The `has_container_queries()` answer of the last `register_containers` call. Re-evaluated on
    // every rebuild, never cached across one.
    bool gate_open_ = false;
};

} // namespace tessera::ui::layout
