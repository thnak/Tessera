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
// Out of scope for AF3 and owned by later slices: startup phases P1-P5 and launch modes (AF5), the
// per-window child scope and multi-window (the AF6 prerequisite), and `TaskScope`/timers (AF2).
//
// Threading and blocking: every method runs on the UI thread. Nothing waits, sleeps, joins or
// detaches. A transition is one bounded loop over at most `max_services` observers, and termination
// progresses only through the caller's `tick()`, which is AF1's contract.

#pragma once

#include "tessera/app/lifecycle_sink.hpp"
#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/service.hpp"
#include "tessera/appfw/service_runner.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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
    LifecycleDispatchResult dispatch(const LifecycleEvent& event);

    // The termination gate. See `TerminationOutcome`.
    TerminationOutcome request_termination();

    // --- the layer-0 seam ------------------------------------------------------------------------
    // Adapts an `app::LifecycleSink` onto `dispatch`, forwarding each forwarded window event in
    // poll order. The sink borrows this application and must be destroyed before it.
    [[nodiscard]] std::unique_ptr<app::LifecycleSink> make_lifecycle_sink();

    // Applies the window state a platform reports through the metrics snapshot instead of an event
    // (`WindowHost::apply_metrics` and the deterministic test seams). The flags become the
    // aggregate and the derived state is dispatched when it moved.
    LifecycleDispatchResult apply_window_snapshot(bool focused, bool occluded, bool minimized);

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
    [[nodiscard]] const WindowLifecycle& window() const noexcept {
        return window_;
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

    ServiceRunner* runner_ = nullptr;
    ApplicationConfig config_;
    WindowLifecycle window_{};
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
