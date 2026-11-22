// SPDX-License-Identifier: MIT
//
// The app lifecycle state machine (ADR-050 section 2). See `application.hpp` for the contract and
// `lifecycle.hpp` for the vocabulary; this file owns the transition rules, the ordered dispatch to
// services and the termination gate.

#include "tessera/appfw/application.hpp"

#include <utility>

namespace tessera::appfw {
namespace {

// The runner phase that means "the start sequence is over", which is where `Launching` ends. AF1
// reports a required failure as `Failed` instead of `Running`, so both end the launch step (the
// failure itself is AF1's `required_failure()`, which a host reports; ADR-050 section 4 item 5
// routes it to the root error boundary in a later slice).
//
// An unprepared runner has no start sequence at all - the composition root registered nothing - so
// it counts as finished: an app with no services still has a lifecycle, and waiting forever for a
// phase that cannot move would leave it stuck in `Launching`.
bool start_sequence_finished(const ServiceRunner& runner) noexcept {
    if (!runner.prepared()) {
        return true;
    }
    const ServicePhase phase = runner.phase();
    return phase == ServicePhase::Running || phase == ServicePhase::Failed ||
           phase == ServicePhase::Stopping || phase == ServicePhase::Stopped;
}

} // namespace

Application::Application(ServiceRunner& runner, ApplicationConfig config)
    : runner_(&runner),
      config_(config),
      windows_(config.max_windows == 0 ? 1 : config.max_windows) {
    if (config_.max_services == 0) {
        config_.max_services = 1;
    }
    observers_ = std::make_unique<ObserverSlot[]>(config_.max_services);
    last_dispatch_ = std::make_unique<ServiceId[]>(config_.max_services);
    // The `<App>` subtree describes the primary window (startup-api-v1.md section 10), so it exists
    // from construction: a single-window app never attaches one and every pre-#180 caller keeps its
    // exact behaviour. The capacity is at least 1, so this cannot fail.
    (void)windows_.attach();
}

Application::~Application() = default;

std::optional<WindowId> Application::attach_window(WindowLifecycle initial) {
    return windows_.attach(initial);
}

bool Application::detach_window(WindowId id) {
    return windows_.detach(id);
}

bool Application::attach_observer(ServiceId id, AppLifecycleObserver& observer) {
    if (observer_for(id) != nullptr || observer_count_ >= config_.max_services) {
        return false;
    }
    observers_[observer_count_] = ObserverSlot{id, &observer};
    ++observer_count_;
    return true;
}

AppLifecycleObserver* Application::observer_for(ServiceId id) const noexcept {
    for (std::size_t index = 0; index < observer_count_; ++index) {
        if (observers_[index].id == id) {
            return observers_[index].observer;
        }
    }
    return nullptr;
}

bool Application::launch() {
    if (launched_ || state_ == AppState::Terminating || state_ == AppState::Terminated) {
        return false;
    }
    // An empty runner (an app with no services) still has a lifecycle: the start sequence it
    // reports is simply already finished, and `tick()` leaves `Launching` on its first call.
    if (runner_->prepared() && runner_->phase() == ServicePhase::Idle && !runner_->begin_start()) {
        return false;
    }
    launched_ = true;
    return true;
}

void Application::tick() {
    if (!launched_) {
        return;
    }
    runner_->tick();
    if (state_ == AppState::Launching) {
        if (start_sequence_finished(*runner_)) {
            // The launch step ends here and nowhere else, so `Launching` cannot be skipped by a
            // window event that arrives while services are still starting.
            (void)enter(derived_state());
        }
        return;
    }
    if (state_ == AppState::Terminating && runner_->phase() == ServicePhase::Stopped) {
        // The stop sequence drained: the ADR's final edge.
        (void)enter(AppState::Terminated);
    }
}

void Application::refresh_state() {
    if (!launched_ || state_ == AppState::Launching || state_ == AppState::Terminated) {
        // `Launching` ends through `tick()`, which is what observes the start sequence, and
        // `Terminated` is final. A window event can therefore not skip the launch step and cannot
        // resurrect a finished app.
        return;
    }
    if (state_ == AppState::Terminating) {
        // Still Terminating: `tick()` owns the edge to Terminated (it is the stop sequence
        // draining, not a window snapshot, that ends the app).
        return;
    }
    (void)enter(derived_state());
}

LifecycleDispatchResult Application::dispatch(const LifecycleEvent& event) {
    return dispatch(kPrimaryWindow, event);
}

LifecycleDispatchResult Application::dispatch(WindowId id, const LifecycleEvent& event) {
    switch (event.signal) {
    case LifecycleSignal::Close: {
        (void)windows_.fold(id, event.signal);
        if (config_.quit_on_last_window_closed) {
            // ADR-050 section 2's table: closing the last window is a termination request, not a
            // `Hidden` state. "Last" is the whole set, not this window: closing one of two windows
            // leaves the app `Active`/`Inactive` on the survivor, which is what makes a
            // multi-window app usable. A `Defer` answer leaves the app where it is (`deferring()`
            // is set), so this reports the state the app is actually in.
            if (windows_.open_count() == 0) {
                const AppState before = state_;
                (void)request_termination();
                return LifecycleDispatchResult{state_, state_ != before, last_dispatch_size_};
            }
            return enter(derived_state());
        }
        return enter(derived_state());
    }
    case LifecycleSignal::ShutdownRequest: {
        const AppState before = state_;
        (void)request_termination();
        return LifecycleDispatchResult{state_, state_ != before, last_dispatch_size_};
    }
    case LifecycleSignal::Suspend:
    case LifecycleSignal::Resume: {
        suspended_ = event.signal == LifecycleSignal::Suspend;
        return enter(derived_state());
    }
    case LifecycleSignal::Focus:
    case LifecycleSignal::Blur:
    case LifecycleSignal::Minimize:
    case LifecycleSignal::Occlude:
    case LifecycleSignal::Restore:
        (void)windows_.fold(id, event.signal);
        return enter(derived_state());
    }
    return LifecycleDispatchResult{state_, false, 0};
}

LifecycleDispatchResult Application::apply_window_snapshot(bool focused, bool occluded,
                                                           bool minimized) {
    return apply_window_snapshot(kPrimaryWindow, focused, occluded, minimized);
}

LifecycleDispatchResult Application::apply_window_snapshot(WindowId id, bool focused, bool occluded,
                                                           bool minimized) {
    (void)windows_.apply_snapshot(id, focused, occluded, minimized);
    return enter(derived_state());
}

LifecycleDispatchResult Application::enter(AppState next) {
    if (state_ == AppState::Terminated) {
        // The final state (ADR-050 section 2): nothing moves the app out of Terminated.
        return LifecycleDispatchResult{state_, false, 0};
    }
    if (state_ == AppState::Terminating && next != AppState::Terminated) {
        // The one-way edge: the only transition out of Terminating is the stop sequence finishing.
        return LifecycleDispatchResult{state_, false, 0};
    }
    if (next == state_) {
        // The event carried no transition: a repeated `Blur`, a `Restore` into a still-occluded
        // window. Services are not told, because `on_app_state` reports states, not events.
        return LifecycleDispatchResult{state_, false, 0};
    }
    state_ = next;
    ++transitions_;
    std::size_t delivered = 0;
    notify_observers(next, delivered);
    return LifecycleDispatchResult{state_, true, delivered};
}

void Application::notify_observers(AppState state, std::size_t& delivered) noexcept {
    last_dispatch_size_ = 0;
    const std::span<const ServiceId> order = runner_->start_order();
    // The runner's start order is the ADR's dependency order (dependencies first). A graceful stop
    // walks it backwards, exactly like `stop()` (section 4 item 1), so a dependent suspends before
    // the dependency it reads and resumes after it.
    for (std::size_t step = 0; step < order.size(); ++step) {
        const std::size_t index = dispatches_in_reverse(state) ? order.size() - 1 - step : step;
        const ServiceId id = order[index];
        // Only a service that actually reached `Started` observes a transition: a service that
        // never ran (Failed, Abandoned, still Registered) has nothing to react to, and AF1 never
        // calls `stop()` on one either.
        if (runner_->state_of(id) != ServiceState::Started) {
            continue;
        }
        AppLifecycleObserver* observer = observer_for(id);
        if (observer == nullptr) {
            continue;
        }
        // The table is sized by `config_.max_services`, the runner by its own; the smaller bound
        // wins so a misconfigured pair can never write past the vector.
        if (last_dispatch_size_ < config_.max_services) {
            last_dispatch_[last_dispatch_size_] = id;
            ++last_dispatch_size_;
        }
        ++delivered;
        try {
            observer->on_app_state(state);
        } catch (...) {
            // ADR-050 section 4 item 5: an exception escaping a callback is caught at the service
            // boundary and never reaches the frame loop. The transition itself stands: a service
            // that cannot handle a state is a service bug, not a reason to un-enter the state.
        }
    }
}

TerminationOutcome Application::request_termination() {
    if (state_ == AppState::Terminating || state_ == AppState::Terminated) {
        return TerminationOutcome::AlreadyTerminating;
    }
    // The vote runs in reverse start order, the order the stop sequence will use, so a dependent is
    // asked before the dependency it would read while saving.
    const std::span<const ServiceId> order = runner_->start_order();
    for (std::size_t step = 0; step < order.size(); ++step) {
        const ServiceId id = order[order.size() - 1 - step];
        if (runner_->state_of(id) != ServiceState::Started) {
            continue;
        }
        AppLifecycleObserver* observer = observer_for(id);
        if (observer == nullptr) {
            continue;
        }
        TerminateVote vote = TerminateVote::Allow;
        try {
            vote = observer->on_terminate_requested();
        } catch (...) {
            // A throwing vote is an unconfirmed answer, and shutdown must not hang on it. `Defer`
            // is the conservative reading: it keeps the app alive (nothing is lost) and reports the
            // deferral, which is exactly what the ADR's "unsaved work" case surfaces.
            vote = TerminateVote::Defer;
        }
        if (vote == TerminateVote::Defer) {
            deferring_ = true;
            return TerminationOutcome::Deferred;
        }
    }
    deferring_ = false;
    const AppState before = state_;
    state_ = AppState::Terminating;
    if (before != AppState::Terminating) {
        ++transitions_;
        std::size_t delivered = 0;
        notify_observers(AppState::Terminating, delivered);
    }
    // AF1's bounded reverse stop (500 ms per service inside 2 s, ADR-050 section 4 item 4). It can
    // only begin from `Running`/`Failed`; a runner still `Starting` is refused, and the app stays
    // `Terminating` with its services running, which `tick()` reports rather than pretending the
    // stop happened.
    (void)runner_->begin_stop();
    return TerminationOutcome::Started;
}

} // namespace tessera::appfw
