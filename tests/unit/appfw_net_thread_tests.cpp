// SPDX-License-Identifier: MIT
//
// The "UI thread never blocks" group (docs/specs/networking-v1.md section 3 item 1 and section 5's
// required case; issue #197). This is the acceptance criterion the issue states first, so it is
// asserted at the seam rather than argued in a comment:
//
//   * `submit` posts to the `IoTaskExecutor` and returns; nothing has reached the back end yet, so
//   the
//     calling thread cannot have performed any network work;
//   * the fake's `start` runs on the executor's stage, and its completion only becomes visible
//   after
//     the completion stage runs;
//   * a frame tick completes while a request is pending, and `RuntimeLoop` keeps idling with zero
//     presents - section 5's "A static scene with a pending request still idles with zero
//     presents".
//
// The last case uses the real `RuntimeLoop` with a `ManualClock`, so "zero presents" is a property
// of the loop rather than of this module: a pending request is not a render cause.

#include "appfw_net_test_support.hpp"
#include "tessera/runtime/runtime_loop.hpp"

#include <cstddef>
#include <string_view>

namespace net_test {

int run_thread_tests();

namespace {

// The calling thread does no work: `submit` returns with the request queued, the back end
// untouched.
void submit_returns_before_any_network_work() {
    Harness harness;
    harness.fake.push_response(response_with(200, "ok"));
    const NetTicket ticket = submit(harness, request_for("https://api.example/x"));

    expect(harness.client.status_of(ticket).state == appfw::NetRequestState::Idle,
           "verbatim: submit returns with the request Idle, so the caller did no network work");
    expect(harness.fake.received().empty(),
           "verbatim: no request reached the back end inside submit");
    expect(harness.executor.pending_work() == 1,
           "the request is queued on the I/O executor, not performed on the calling thread");
}

// The stages stay separate: the executor's work body, then its completion, then the transport's
// completion - and the caller hears nothing until the last one.
void the_attempt_runs_on_the_executor_not_the_caller() {
    Harness harness;
    harness.fake.push_response(response_with(200, "ok"));
    int deliveries = 0;
    auto ticket = harness.client.submit(request_for("https://api.example/x"), {},
                                        [&deliveries](NetResult) { ++deliveries; });
    expect(ticket.has_value(), "the fixture submits");

    expect(harness.executor.pending_work() == 1, "one I/O body is queued");
    expect(deliveries == 0, "no callback ran before the I/O body did");

    harness.executor.run_one();
    expect(harness.fake.received().size() == 1, "the body is what reached the back end");
    expect(harness.client.status_of(*ticket).state == appfw::NetRequestState::Idle,
           "the request is still Idle until the executor's completion is drained");
    expect(deliveries == 0, "the transport has not completed yet");

    harness.executor.run_completion_one();
    expect(harness.client.status_of(*ticket).state == appfw::NetRequestState::Loading,
           "the executor's completion moves the request to Loading");

    harness.fake.deliver_due();
    expect(deliveries == 0, "the transport's result is posted, never delivered inline");
    harness.completions.run(8);
    expect(deliveries == 1, "the completion queue is what delivers the result");
    expect(harness.client.status_of(*ticket).state == appfw::NetRequestState::Done,
           "the request is Done once the completion ran");
}

// A frame tick runs to completion while the request is outstanding, and the request is still there
// afterwards: no tick waits for the network, and no tick loses the request.
void a_frame_tick_completes_while_a_request_is_pending() {
    Harness harness;
    harness.fake.push_response(response_with(200, "ok"), 5000);
    const NetTicket ticket = submit(harness, request_for("https://api.example/slow"), 30000);
    pump(harness);
    expect(harness.client.status_of(ticket).state == appfw::NetRequestState::Loading,
           "the request is in flight across the tick");

    // A whole tick's worth of unrelated work - the point is that it finishes.
    std::size_t ticks = 0;
    for (int frame = 0; frame < 60; ++frame) {
        harness.client.tick();
        pump(harness, 4);
        ++ticks;
    }
    expect(ticks == 60, "sixty frame ticks completed while the request was pending");
    expect(harness.client.status_of(ticket).state == appfw::NetRequestState::Loading,
           "verbatim: a frame tick completes while a request is pending, and the request survives");

    advance(harness, 6.0);
    expect(harness.client.status_of(ticket).state == appfw::NetRequestState::Done,
           "the request completes once its delay elapses on the injected clock");
}

// Section 5: "A static scene with a pending request still idles with zero presents." The real
// `RuntimeLoop` is driven here, with the module's completion queue attached, so this is the loop's
// own render-on-demand policy rather than a restatement of it.
void a_pending_request_is_not_a_render_cause() {
    runtime::ManualClock clock;
    runtime::CompletionQueue completions{64};
    runtime::TimerQueue timers{8};
    runtime::ManualIoExecutor executor{8};
    FakeNet fake{completions, clock};
    fake.push_response(response_with(200, "ok"), 5000);
    NetClient client(NetClientConfig{}, fake, executor, completions, timers, clock);

    runtime::RuntimeLoopConfig config;
    config.completion_queue = &completions;
    config.timers = &timers;
    config.render_on_demand = true;
    runtime::RuntimeLoop loop(clock, config);

    auto ticket = client.submit(request_for("https://api.example/slow"), {}, {});
    expect(ticket.has_value(), "the fixture submits");

    // The pending request is drained into Loading, but a request in flight requests no frame.
    const runtime::RuntimeTick first = loop.tick();
    executor.run_one();
    executor.run_completion_one();
    fake.deliver_due();
    const runtime::RuntimeTick second = loop.tick();
    (void)first;
    (void)second;

    // A request with no completed work and no timer due leaves the loop parked.
    std::size_t presents = 0;
    for (int frame = 0; frame < 30; ++frame) {
        clock.advance(1.0 / 60.0);
        const runtime::RuntimeTick tick = loop.tick();
        if (tick.should_render) {
            ++presents;
        }
        // No timer is due within these frames: the deadline is 30 s away.
        expect(loop.next_timer_wait_seconds() > 1.0,
               "the pending request's deadline is far away, so the host may park");
    }
    expect(presents == 0,
           "verbatim: a static scene with a pending request idles with zero presents");
    expect(client.status_of(*ticket).state == appfw::NetRequestState::Loading,
           "the request was pending the whole time");
}

// The whole point of the seam: the timeout is a timer the loop fires, so a host that parks on the
// deadline wakes exactly once and finds a resolved request.
void the_deadline_wakes_the_loop_with_a_timer() {
    runtime::ManualClock clock;
    runtime::CompletionQueue completions{64};
    runtime::TimerQueue timers{8};
    runtime::ManualIoExecutor executor{8};
    FakeNet fake{completions, clock};
    fake.push_response(response_with(200, "never"), 60000);
    NetClient client(NetClientConfig{}, fake, executor, completions, timers, clock);

    runtime::RuntimeLoopConfig config;
    config.completion_queue = &completions;
    config.timers = &timers;
    runtime::RuntimeLoop loop(clock, config);

    auto ticket = client.submit(request_for("https://api.example/slow"), {}, {});
    expect(ticket.has_value(), "the fixture submits");
    loop.tick();
    executor.run_one();
    executor.run_completion_one();
    loop.tick();

    // The deadline is 30 s out, so the parking wait is just under 30 s: a real timeout, not a spin.
    const double wait = loop.next_timer_wait_seconds();
    expect(wait > 29.0 && wait <= 30.0,
           "verbatim: the retry/timeout deadline is a timer the host can park on, not a sleep");
    expect(timers.size() == 1, "exactly one deadline timer is armed for the request");

    clock.advance(wait);
    const runtime::RuntimeTick tick = loop.tick();
    expect(tick.timers_fired == 1, "the loop fired the deadline timer");
    expect(client.status_of(*ticket).state == appfw::NetRequestState::Error,
           "the deadline resolved the request as an error");
    expect(client.status_of(*ticket).outcome == appfw::NetOutcome::Timeout,
           "the resolved outcome is Timeout");
}

// The bounded concurrency of services.md section 5 is a refusal, not a queue: the caller chose how
// many screens may fetch at once, so the ninth request of eight is told so immediately.
void the_in_flight_bound_refuses_rather_than_queues() {
    NetClientConfig config;
    config.max_in_flight = 2;
    Harness harness(config);
    for (int index = 0; index < 4; ++index) {
        harness.fake.push_response(response_with(200), 1000);
    }
    auto first = harness.client.submit(request_for("https://api.example/1"), {}, {});
    auto second = harness.client.submit(request_for("https://api.example/2"), {}, {});
    auto third = harness.client.submit(request_for("https://api.example/3"), {}, {});
    expect(first.has_value() && second.has_value(), "the first two requests are accepted");
    expect(!third.has_value(), "the third is refused at the bound");
    expect(third.error().kind == NetErrorKind::Other, "a refusal is the client's own error");
    expect(harness.client.in_flight() == 2, "two requests are in flight, and no third is queued");
    expect(harness.fake.received().empty(), "a refused request never reached the back end");
}

} // namespace

int run_thread_tests() {
    submit_returns_before_any_network_work();
    the_attempt_runs_on_the_executor_not_the_caller();
    a_frame_tick_completes_while_a_request_is_pending();
    a_pending_request_is_not_a_render_cause();
    the_deadline_wakes_the_loop_with_a_timer();
    the_in_flight_bound_refuses_rather_than_queues();
    return g_failures;
}

} // namespace net_test
