// SPDX-License-Identifier: MIT
//
// `Application`: the app lifecycle state machine and the ordered dispatch of lifecycle transitions
// to services (ADR-050 section 2; AF3 of the roadmap).
//
// What this slice implements, stated because ADR-050 section 2 also names startup P1-P5, the root
// task scope and per-window scopes:
//
//   * the state machine of section 2, driven by normalized (`LifecycleSignal`) events only;
//   * ordered dispatch of a transition to the services that reached `Started`, in the runner's
//   start
//     order (dependencies first), one transition at a time and synchronously, so every service
//     observes event N before any service observes event N+1;
//   * the termination gate: `request_termination()` asks the started services
//     `on_terminate_requested()`; one `Defer` answer aborts the sequence and is reported, an
//     all-`Allow` sequence begins `ServiceRunner`'s existing reverse stop;
//   * the layer-0 seam: `make_lifecycle_sink()` adapts `app::LifecycleSink`, so `WindowHost`
//   forwards
//     focus/minimize/occlusion without appfw naming the window driver.
//
// Multi-window (issue #180) is the window set: `attach_window()` registers a window and returns its
// `WindowId`, the `WindowId` overloads of `dispatch()`/`apply_window_snapshot()` fold one window's
// events, and the state is derived from the aggregate over all of them (ADR-050 section 2 derives
// every state from a phrase about the whole set). The no-`WindowId` overloads address
// `kPrimaryWindow`, which is the window the `<App>` subtree describes, so a single-window app is
// unchanged. See `window_registry.hpp` for the aggregation rule.
//
// Out of scope for AF3 and owned by later slices: startup phases P1-P5 and launch modes (AF5), the
// per-window child service scope, and `TaskScope`/timers (AF2).
//
// Threading and blocking: every method runs on the UI thread. Nothing waits, sleeps, joins or
// detaches. A transition is one bounded loop over at most `max_services` observers, and termination
// progresses only through the caller's `tick()`, which is AF1's contract.

#pragma once

#include "tessera/app/lifecycle_sink.hpp"
#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/service.hpp"
#include "tessera/appfw/service_requirements.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/appfw/window_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace tessera::appfw {

// ADR-050 section 4's two lifecycle callbacks. They are a separate interface rather than two more
// pure virtuals on `Service`, so AF1's landed contract (start/stop) is unchanged and a service opts
// in explicitly:
//
//   class MailService final : public Service, public AppLifecycleObserver { ... };
//
// Virtual dispatch at this boundary is the section 4 item 6 decision; nothing per frame crosses it.
class AppLifecycleObserver {
public:
    AppLifecycleObserver() = default;
    AppLifecycleObserver(const AppLifecycleObserver&) = delete;
    AppLifecycleObserver& operator=(const AppLifecycleObserver&) = delete;
    virtual ~AppLifecycleObserver() = default;

    // The app entered `state`. Called once per transition per observer, on the UI thread, and only
    // while the observing service is `Started`. It must not block; work that outlives the state
    // belongs in the service's own task scope.
    virtual void on_app_state(AppState state) = 0;

    // A termination request is being considered. `Allow` lets shutdown proceed; `Defer` says
    // unsaved work exists, which cancels the sequence and is reported to the app (ADR-050 section
    // 2).
    [[nodiscard]] virtual TerminateVote on_terminate_requested() {
        return TerminateVote::Allow;
    }
};

// What one lifecycle event did. `entered` is false when the event carried no transition (a repeated
// `Blur`, a `Restore` while still occluded); `delivered` is how many observers received it.
struct LifecycleDispatchResult {
    AppState state = AppState::Launching;
    bool entered = false;
    std::size_t delivered = 0;
};

struct ApplicationConfig {
    // ADR-050 section 2's `quitOnLastWindowClosed`: with the default, closing the last window is a
    // termination request. With it false the window closes and the app stays `Hidden` (a tray app).
    bool quit_on_last_window_closed = true;
    // Bound on the observer table, so `attach_observer` never allocates and dispatch never walks
    // more services than the table holds. Matches AF1's `ServiceRunnerConfig::max_services`
    // default.
    std::size_t max_services = 32;
    // Bound on the window set (issue #180), so `attach_window` never allocates. Matches
    // `kMaxWindowsPerApplication`, which is also the renderer frame ring's window bound.
    std::size_t max_windows = kMaxWindowsPerApplication;
};

// The outcome of `request_termination()`.
enum class TerminationOutcome : std::uint8_t {
    // No started service deferred: the runner's stop sequence has begun (`phase()` is `Stopping`).
    Started,
    // At least one service returned `Defer`: nothing stopped and no state changed. The app is told
    // through `deferring()` and may retry after saving (ADR-050 section 2).
    Deferred,
    // Already `Terminating`/`Terminated`, or the runner refused to begin its stop sequence.
    AlreadyTerminating,
};

[[nodiscard]] constexpr std::string_view
termination_outcome_name(TerminationOutcome outcome) noexcept {
    switch (outcome) {
    case TerminationOutcome::Started:
        return "started";
    case TerminationOutcome::Deferred:
        return "deferred";
    case TerminationOutcome::AlreadyTerminating:
        return "already-terminating";
    }
    return "unknown";
}

class Application {
public:
    // `runner` is borrowed and must outlive the application. It must be `prepare()`d before
    // `launch()` (and remains usable when empty: an app with no services still has a lifecycle).
    explicit Application(ServiceRunner& runner, ApplicationConfig config = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Associates a service id with its lifecycle observer. Call it once per service before
    // `launch()`, from the composition root; a duplicate id, a full table or a null observer is
    // refused. The observer must outlive the application.
    bool attach_observer(ServiceId id, AppLifecycleObserver& observer);

    // ADR-050 section 3.1 item 6: the services the mounted components require (the concatenated
    // `required_services()` of the generated bases) and the scope that must provide them. Call it
    // from the composition root before `launch()`; `launch()` then refuses to start when any
    // requirement has no provider, after reporting each as `TESSERA_APPFW_SERVICE_MISSING` with the
    // component and TSX location. Both are borrowed and must outlive the application. Without this
    // call nothing is checked, which keeps an app with no `useService` unchanged.
    void require_services(const ServiceContext& context,
                          std::span<const RequiredService> required) noexcept;

    // --- the window set (issue #180) -------------------------------------------------------------
    // Registers a window and returns its id, or nullopt when the set is full. The primary window is
    // registered by the constructor, so a single-window app never calls this. A window is detached
    // when it is destroyed; ids are never reused.
    std::optional<WindowId> attach_window(WindowLifecycle initial = {});
    // Removes a window from the set. False when the id is unknown. Detaching the last open window
    // does not by itself request termination: the platform reports the close, and `dispatch` owns
    // that rule.
    bool detach_window(WindowId id);

    // The window set, for a caller that walks it (a host that owns several windows).
    [[nodiscard]] const WindowRegistry& windows() const noexcept {
        return windows_;
    }
    // The aggregate over every registered window: what `derive_app_state` consumes. This is the
    // multi-window form of `window()`.
    [[nodiscard]] WindowLifecycle aggregate_window() const noexcept {
        return windows_.aggregate();
    }

    // --- startup and the state machine -----------------------------------------------------------
    // Begins the runner's start sequence and stays in `Launching`. False when the runner is not
    // prepared or is no longer idle. The app leaves `Launching` on the first `tick()` that observes
    // the start sequence finished; `refresh_state()` asks that question directly.
    bool launch();

    // Bounded progression: drives the runner's deadline checks, leaves `Launching` once the start
    // sequence finished, and moves `Terminating` to `Terminated` once the stop sequence finished.
    // Never blocks; the completion queue runs the service coroutines.
    void tick();

    // Recomputes the state from the window aggregate and the suspension flag and dispatches when
    // the state moved. Called by `tick()`; public so a host that owns its own loop can ask after a
    // window snapshot.
    void refresh_state();

    // One normalized lifecycle event: folds it into the window aggregate, recomputes the state, and
    // (when the state moved) dispatches `on_app_state` in start order. `Close` becomes a
    // termination request when `quitOnLastWindowClosed` is set, `ShutdownRequest` always does,
    // `Suspend`/`Resume` set and clear the suspension flag.
    //
    // The no-`WindowId` overload addresses `kPrimaryWindow` (the `<App>` subtree's window), which
    // is what every pre-#180 caller means. The `WindowId` overload folds the event into that one
    // window and derives the state from the aggregate over all of them.
    LifecycleDispatchResult dispatch(const LifecycleEvent& event);
    LifecycleDispatchResult dispatch(WindowId id, const LifecycleEvent& event);

    // The termination gate. See `TerminationOutcome`.
    TerminationOutcome request_termination();

    // --- the layer-0 seam ------------------------------------------------------------------------
    // Adapts an `app::LifecycleSink` onto `dispatch`, forwarding each forwarded window event in
    // poll order. The sink borrows this application and must be destroyed before it.
    //
    // The `WindowId` overload is the multi-window form: one sink per window, so a driver that owns
    // several windows forwards each one's events into its own slot of the set. A single-window app
    // installs the no-argument sink, which addresses `kPrimaryWindow`.
    [[nodiscard]] std::unique_ptr<app::LifecycleSink> make_lifecycle_sink();
    [[nodiscard]] std::unique_ptr<app::LifecycleSink> make_lifecycle_sink(WindowId id);

    // Applies the window state a platform reports through the metrics snapshot instead of an event
    // (`WindowHost::apply_metrics` and the deterministic test seams). The flags become that
    // window's aggregate and the derived state is dispatched when it moved. The no-`WindowId`
    // overload addresses `kPrimaryWindow`.
    LifecycleDispatchResult apply_window_snapshot(bool focused, bool occluded, bool minimized);
    LifecycleDispatchResult apply_window_snapshot(WindowId id, bool focused, bool occluded,
                                                  bool minimized);

    // --- observation -----------------------------------------------------------------------------
    [[nodiscard]] AppState state() const noexcept {
        return state_;
    }
    [[nodiscard]] bool deferring() const noexcept {
        return deferring_;
    }
    [[nodiscard]] bool suspended() const noexcept {
        return suspended_;
    }
    [[nodiscard]] bool launched() const noexcept {
        return launched_;
    }
    // The primary window's aggregate (`kPrimaryWindow`). A multi-window caller reads
    // `aggregate_window()` instead; this stays because it is what a single-window caller and every
    // pre-#180 test mean by "the window". A detached primary window reads as the default aggregate
    // rather than dangling.
    [[nodiscard]] const WindowLifecycle& window() const noexcept {
        const WindowLifecycle* primary = windows_.find(kPrimaryWindow);
        return primary != nullptr ? *primary : detached_window_;
    }
    [[nodiscard]] const ApplicationConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] ServiceRunner& runner() noexcept {
        return *runner_;
    }
    // How many state transitions were entered (events that arrived without one do not count).
    [[nodiscard]] std::size_t transitions() const noexcept {
        return transitions_;
    }
    // The services that observed the most recent transition, in the order they were called: the
    // ordered-event vector an acceptance test asserts on. Empty when the last event did not move
    // the state.
    [[nodiscard]] std::span<const ServiceId> last_dispatch() const noexcept {
        return std::span<const ServiceId>(last_dispatch_.get(), last_dispatch_size_);
    }

private:
    struct ObserverSlot {
        ServiceId id{};
        AppLifecycleObserver* observer = nullptr;
    };

    LifecycleDispatchResult enter(AppState next);
    LifecycleDispatchResult synthesize(LifecycleSignal signal);
    void notify_observers(AppState state, std::size_t& delivered) noexcept;
    [[nodiscard]] AppLifecycleObserver* observer_for(ServiceId id) const noexcept;
    // The state the aggregate derives right now. One call site for the rule, so the one-window and
    // N-window paths cannot drift.
    [[nodiscard]] AppState derived_state() const noexcept {
        return derive_app_state(windows_.aggregate(), suspended_);
    }

    ServiceRunner* runner_ = nullptr;
    const ServiceContext* required_context_ = nullptr;
    std::span<const RequiredService> required_services_;
    ApplicationConfig config_;
    // The app's window set (issue #180). The primary window is registered by the constructor, so a
    // single-window app's aggregate is exactly the one `WindowLifecycle` AF3 had.
    WindowRegistry windows_;
    // What `window()` reads when the primary window has been detached: a default aggregate rather
    // than a dangling reference. Never written.
    WindowLifecycle detached_window_{};
    AppState state_ = AppState::Launching;
    bool suspended_ = false;
    bool launched_ = false;
    bool deferring_ = false;
    std::size_t transitions_ = 0;
    std::unique_ptr<ObserverSlot[]> observers_;
    std::size_t observer_count_ = 0;
    std::unique_ptr<ServiceId[]> last_dispatch_;
    std::size_t last_dispatch_size_ = 0;
};

} // namespace tessera::appfw
