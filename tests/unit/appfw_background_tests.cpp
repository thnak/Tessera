// SPDX-License-Identifier: MIT
//
// `appfw::BackgroundCoordinator` (issue #453): the in-process background policy. Deterministic:
// a `ManualClock`, a hand-drained `CompletionQueue` and a `TimerQueue` fed explicit instants, so
// there is no wall clock, sleep or thread.

#include "appfw_task_scope_test_support.hpp"
#include "tessera/appfw/background.hpp"

namespace appfw_task_test {

namespace {

using appfw::AppState;
using appfw::BackgroundPolicy;

appfw::BackgroundRegistration reg(appfw::TaskScope& scope, BackgroundPolicy policy,
                                  std::function<void()> checkpoint = {}) {
    return appfw::BackgroundRegistration{&scope, policy, std::move(checkpoint)};
}

bool checkpoint_runs_once_per_background_entry() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(16);
    appfw::TaskScope scope(clock, queue);
    appfw::BackgroundCoordinator background;
    int saved = 0;
    ok = expect(background.register_scope(reg(scope, BackgroundPolicy::Continue, [&] { ++saved; }))
                    .has_value(),
                "registers") &&
         ok;
    background.on_app_state(AppState::Active);
    ok =
        expect(saved == 0 && !background.backgrounded(), "visible state checkpoints nothing") && ok;
    background.on_app_state(AppState::Hidden);
    background.on_app_state(AppState::Suspended);
    ok = expect(saved == 1, "Hidden then Suspended is one background entry") && ok;
    background.on_app_state(AppState::Active);
    background.on_app_state(AppState::Hidden);
    ok = expect(saved == 2 && background.checkpoints_run() == 2,
                "a second entry checkpoints again") &&
         ok;
    // Direct Active -> Suspended (no Hidden) still checkpoints.
    background.on_app_state(AppState::Active);
    background.on_app_state(AppState::Suspended);
    ok = expect(saved == 3, "Suspended alone checkpoints") && ok;
    return ok;
}

bool suspend_cancels_only_non_continue_scopes() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    appfw::TaskScope keep(clock, queue);
    appfw::TaskScope pause(clock, queue);
    appfw::TaskScope throttle(clock, queue);
    appfw::BackgroundCoordinator background;
    (void)background.register_scope(reg(keep, BackgroundPolicy::Continue));
    (void)background.register_scope(reg(pause, BackgroundPolicy::Pause));
    (void)background.register_scope(reg(throttle, BackgroundPolicy::Throttle));
    ChildLog keep_log;
    ChildLog pause_log;
    (void)keep.spawn(hop_child(keep.context(), queue, keep_log, 3));
    (void)pause.spawn(hop_child(pause.context(), queue, pause_log, 3));
    pump_until(queue, [&] { return keep_log.entries == 1 && pause_log.entries == 1; });

    background.on_app_state(AppState::Hidden);
    ok = expect(!keep.cancelled() && !pause.cancelled(), "Hidden cancels nothing") && ok;
    background.on_app_state(AppState::Suspended);
    ok = expect(!keep.cancelled(), "Continue survives Suspended") && ok;
    ok =
        expect(pause.cancelled() && throttle.cancelled(), "Pause and Throttle are cancelled") && ok;
    ok = expect(background.scopes_cancelled() == 2, "two cancels counted") && ok;
    pump(queue);
    ok = expect(pause_log.completions == 0 && pause_log.steps == 0,
                "the cancelled child never advances") &&
         ok;
    ok = expect(keep_log.completions == 1, "the Continue child finishes") && ok;
    ok = expect(pause.drained(), "the cancelled scope drains") && ok;
    // Repeating the state must not double count.
    background.on_app_state(AppState::Suspended);
    ok = expect(background.scopes_cancelled() == 2, "Suspended is idempotent") && ok;
    return ok;
}

bool suspend_pauses_timers_and_resume_fires_overdue() {
    bool ok = true;
    runtime::TimerQueue timers(4);
    appfw::BackgroundCoordinator background(&timers);
    int one_shot = 0;
    int periodic = 0;
    (void)timers.schedule_one_shot(1.0, [&] { ++one_shot; });
    (void)timers.schedule_periodic(2.0, 1.0, [&] { ++periodic; });

    background.on_app_state(AppState::Suspended);
    ok = expect(timers.paused(), "timers pause on Suspended") && ok;
    ok = expect(timers.next_deadline_seconds() == runtime::kNoDeadline,
                "a paused queue gives the loop nothing to park for") &&
         ok;
    ok = expect(timers.fire_due(100.0) == 0 && one_shot == 0, "nothing fires while paused") && ok;
    ok = expect(timers.size() == 2, "paused timers are kept, not dropped") && ok;

    background.on_app_state(AppState::Inactive);
    ok = expect(!timers.paused(), "timers resume when Suspended is left") && ok;
    ok = expect(timers.fire_due(100.0) == 2, "overdue timers fire once each") && ok;
    ok = expect(one_shot == 1 && periodic == 1, "the periodic timer coalesces, no burst") && ok;
    ok = expect(timers.next_deadline_seconds() == 101.0, "the periodic deadline moved past now") &&
         ok;
    return ok;
}

bool hidden_does_not_pause_timers() {
    runtime::TimerQueue timers(2);
    appfw::BackgroundCoordinator background(&timers);
    (void)timers.schedule_one_shot(1.0, [] {});
    background.on_app_state(AppState::Hidden);
    return expect(!timers.paused() && timers.next_deadline_seconds() == 1.0,
                  "desktop Hidden keeps timers running");
}

bool registration_is_bounded_and_unregister_stops_actions() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(8);
    appfw::TaskScope first(clock, queue);
    appfw::TaskScope second(clock, queue);
    appfw::BackgroundCoordinator background(nullptr, 1);
    const auto id = background.register_scope(reg(first, BackgroundPolicy::Pause));
    ok = expect(id.has_value(), "first registers") && ok;
    ok = expect(!background.register_scope(reg(second, BackgroundPolicy::Pause)).has_value(),
                "a full coordinator refuses") &&
         ok;
    ok = expect(!background.register_scope(appfw::BackgroundRegistration{}).has_value(),
                "a null scope is refused") &&
         ok;
    ok = expect(background.refused() == 2, "refusals are counted") && ok;
    ok = expect(background.unregister(*id) && !background.unregister(*id),
                "unregister is once-only") &&
         ok;
    background.on_app_state(AppState::Suspended);
    ok = expect(!first.cancelled(), "an unregistered scope is left alone") && ok;
    const auto next = background.register_scope(reg(second, BackgroundPolicy::Pause));
    ok = expect(next.has_value() && *next != *id, "ids are not reused") && ok;
    return ok;
}

} // namespace

int run_background_tests() {
    const int before = failures;
    checkpoint_runs_once_per_background_entry();
    suspend_cancels_only_non_continue_scopes();
    suspend_pauses_timers_and_resume_fires_overdue();
    hidden_does_not_pause_timers();
    registration_is_bounded_and_unregister_stops_actions();
    return failures - before;
}

} // namespace appfw_task_test

int main() {
    const int failures = appfw_task_test::run_background_tests();
    if (failures != 0) {
        std::cerr << "appfw_background: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_background: ok\n";
    return 0;
}
