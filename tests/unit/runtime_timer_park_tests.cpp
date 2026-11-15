// SPDX-License-Identifier: MIT
//
// The layer-0 timer wake source, part 2 (AF2, issue #174; ADR-050 section 5 item 2): the parking
// acceptance clause, verbatim from the issue:
//
//   "an idle loop with only a far timer parks (zero presents, no polling)."
//
// It is proved with counters, never with wall-clock time. The loop is ticked by hand against a
// `ManualClock`, so "it parked" means "the loop reported no frame, and the host's wait budget came
// from `next_timer_wait_seconds()` instead of a re-tick". Nothing here sleeps, spins, or starts a
// thread.
//
// Ordering and handle safety are in `runtime_timer_tests.cpp`.

#include "runtime_timer_test_support.hpp"

namespace runtime_timer_test {

// --- an idle loop with only a far timer parks -------------------------------------------------
bool an_idle_loop_with_a_far_timer_parks() {
    bool ok = true;
    ManualClock clock(0.0);
    TimerQueue timers(8);
    RuntimeLoop loop = make_loop(clock, timers);

    int presents = 0;
    int ticks = 0;
    int idle_ticks = 0;
    int fired = 0;
    std::size_t fired_total = 0;

    (void)timers.schedule_one_shot(10.0, [&] {
        ++fired;
        // A timer never forces a frame: it schedules work like any completion (ADR-050 section 5
        // item 2). This one only records that it ran.
    });

    // First tick at t = 0. The only pending work is a timer 10 seconds out, so the loop must report
    // idle and no frame; the host's park budget is the timer's wait.
    RuntimeTick tick = step(loop, presents);
    ++ticks;
    if (tick.idle) {
        ++idle_ticks;
    }
    fired_total += tick.timers_fired;
    ok = expect(!tick.should_render, "the first tick presents nothing") && ok;
    ok = expect(tick.idle, "the first tick is idle") && ok;
    ok = expect(tick.timers_fired == 0, "a far timer does not fire at t = 0") && ok;
    ok =
        expect(near(loop.next_timer_wait_seconds(), 10.0), "the loop reports a 10 s park budget") &&
        ok;

    // Park, then wake on the timer's deadline. This is the host's whole iteration: no polling ticks
    // in between, so `ticks` advances by exactly one.
    clock.advance(loop.next_timer_wait_seconds());
    tick = step(loop, presents);
    ++ticks;
    if (tick.idle) {
        ++idle_ticks;
    }
    fired_total += tick.timers_fired;
    ok = expect(fired == 1, "the timer fired when the clock reached its deadline") && ok;
    ok = expect(tick.timers_fired == 1, "the tick reports the timer it fired") && ok;
    ok = expect(!tick.should_render, "a timer alone does not present") && ok;
    ok = expect(tick.idle, "the loop is idle again right after the timer") && ok;

    // The concrete acceptance evidence: two ticks for ten seconds of parking, zero presents.
    ok = expect(presents == 0, "an idle loop with only a far timer presents zero frames") && ok;
    ok = expect(ticks == 2, "ten seconds of parking cost two ticks, not a poll loop") && ok;
    ok = expect(idle_ticks == 2, "both ticks were idle") && ok;
    ok = expect(fired_total == 1, "exactly one timer fired across the park") && ok;
    ok = expect(timers.size() == 0, "the one-shot timer is spent") && ok;
    ok = expect(loop.next_timer_wait_seconds() == kNoDeadline,
                "with no timers left the loop reports no park budget from them") &&
         ok;
    return ok;
}

// A due timer asks for a zero wait, so a host that clamps its platform wait to this value cannot
// oversleep it. This is what keeps the loop from polling: the wait is derived from the deadline,
// not from a fixed interval.
bool a_due_timer_asks_for_a_zero_wait_and_fires_without_a_frame() {
    bool ok = true;
    ManualClock clock(0.0);
    TimerQueue timers(4);
    RuntimeLoop loop = make_loop(clock, timers);
    int presents = 0;
    int fired = 0;
    (void)timers.schedule_one_shot(0.0, [&] { ++fired; });

    ok = expect(loop.next_timer_wait_seconds() == 0.0,
                "a timer due now asks for a zero-length wait") &&
         ok;
    const RuntimeTick tick = step(loop, presents);
    ok = expect(fired == 1, "the due timer fired on the first tick") && ok;
    ok = expect(tick.timers_fired == 1, "the tick counted it") && ok;
    ok = expect(presents == 0, "a timer does not present") && ok;
    ok = expect(tick.idle, "the loop is still idle after a timer") && ok;
    return ok;
}

// No queue attached is the layer-0 default: the query must answer "nothing scheduled" instead of
// crashing or inventing a deadline, and a tick must not fire anything.
bool a_loop_without_a_timer_queue_reports_nothing() {
    bool ok = true;
    ManualClock clock(0.0);
    RuntimeLoop loop(clock, RuntimeLoopConfig{});
    ok = expect(loop.timer_queue() == nullptr, "the timer queue is optional") && ok;
    ok = expect(loop.next_timer_wait_seconds() == kNoDeadline,
                "a loop with no timer queue reports no timer deadline") &&
         ok;
    int presents = 0;
    const RuntimeTick tick = step(loop, presents);
    ok = expect(tick.timers_fired == 0, "a tick without a queue fires nothing") && ok;
    ok = expect(tick.idle, "the loop is idle") && ok;
    return ok;
}

// Determinism across a long park with a periodic timer: the loop fires it once per parked period
// and never presents while only timers are pending.
bool a_periodic_timer_wakes_the_loop_without_presenting() {
    bool ok = true;
    ManualClock clock(0.0);
    TimerQueue timers(4);
    RuntimeLoop loop = make_loop(clock, timers);
    int presents = 0;
    int fired = 0;
    (void)timers.schedule_periodic(0.25, 0.25, [&] { ++fired; });

    // Three parks of exactly one period each: one fire per park, no presents, no extra ticks.
    int ticks = 0;
    for (int period = 0; period < 3; ++period) {
        clock.advance(loop.next_timer_wait_seconds());
        const RuntimeTick tick = step(loop, presents);
        ++ticks;
        ok = expect(tick.timers_fired == 1, "one fire per parked period") && ok;
        ok = expect(!tick.should_render, "a periodic timer presents nothing") && ok;
    }
    ok = expect(fired == 3, "three periods fired three times") && ok;
    ok = expect(presents == 0, "no presents across three periodic wakes") && ok;
    ok = expect(ticks == 3, "three wakes, three ticks") && ok;
    ok = expect(near(loop.next_timer_wait_seconds(), 0.25), "the park budget is the next period") &&
         ok;
    return ok;
}

// A timer never becomes a render cause on its own, but work it schedules still reaches the frame:
// the callback posts a completion, the next tick's drain runs it, and only its explicit frame
// request presents. This is the "a timer schedules work like any completion" half of ADR-050
// section 5 item 2, and it keeps the zero-present clause from being satisfied by breaking timers.
bool a_timer_can_schedule_work_that_requests_a_frame() {
    bool ok = true;
    ManualClock clock(0.0);
    TimerQueue timers(4);
    tessera::runtime::CompletionQueue completions(8);
    RuntimeLoopConfig config;
    config.fixed_step = FixedStepConfig{60.0, 250.0, 5, 1.0};
    config.render_on_demand = true;
    config.timers = &timers;
    config.completion_queue = &completions;
    RuntimeLoop loop(clock, config);
    int presents = 0;
    int ran = 0;
    (void)timers.schedule_one_shot(1.0, [&] {
        (void)completions.post(tessera::runtime::CompletionPriority::Visible, [&] {
            ++ran;
            loop.request_frame();
        });
    });

    // Park to the timer, then tick: the timer fires, the completion is queued but not yet drained.
    clock.advance(loop.next_timer_wait_seconds());
    RuntimeTick tick = step(loop, presents);
    ok = expect(tick.timers_fired == 1, "the timer fired on the wake") && ok;
    ok = expect(ran == 0, "the timer's work has not run yet: it is only queued") && ok;
    ok = expect(!tick.should_render, "queueing work does not present in the same tick") && ok;

    // The next tick drains the completion, which asks for a frame.
    tick = step(loop, presents);
    ok = expect(ran == 1, "the next tick drained the timer's completion") && ok;
    ok = expect(tick.should_render, "the completion's frame request presents") && ok;
    ok = expect(presents == 1, "exactly one present, caused by the request") && ok;
    return ok;
}

int run_park_tests() {
    const int before = failures;
    an_idle_loop_with_a_far_timer_parks();
    a_due_timer_asks_for_a_zero_wait_and_fires_without_a_frame();
    a_loop_without_a_timer_queue_reports_nothing();
    a_periodic_timer_wakes_the_loop_without_presenting();
    a_timer_can_schedule_work_that_requests_a_frame();
    return failures - before;
}

} // namespace runtime_timer_test
