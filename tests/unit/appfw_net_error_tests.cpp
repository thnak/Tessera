// SPDX-License-Identifier: MIT
//
// The `net` failure paths (docs/specs/networking-v1.md section 3 items 3, 4, 5 and 6; section 5's
// required cases; issue #197). The acceptance words this group owns are **"cancellation, offline
// and failure paths are tested"** and the requirement that a timeout, a non-2xx status and a
// transport failure are three distinct named outcomes.
//
// Every assertion drives the deterministic stack by hand: `ManualClock` for time,
// `ManualIoExecutor` for the I/O stage, `FakeNet` for the network, `TimerQueue::fire_due` for the
// deadline and `CompletionQueue::run` for delivery. Nothing here waits, sleeps or opens a socket.

#include "appfw_net_test_support.hpp"

#include <string>
#include <string_view>

namespace net_test {

int run_error_tests();

namespace {

// The deadline is a timer on the injected clock, not a sleep: advancing the manual clock and firing
// the queue is what makes it expire, and the outcome is the named `Timeout`.
void a_timeout_is_a_named_outcome() {
    Harness harness;
    harness.fake.push_response(response_with(200, "late"), 5000);
    const NetTicket ticket = submit(harness, request_for("https://api.example/x"), 1000);
    pump(harness);

    expect(harness.client.status_of(ticket).state == appfw::NetRequestState::Loading,
           "the request is still loading before its deadline");

    // 1000 ms, not 5000: the deadline is the request's, and it lands in Error with `Timeout`.
    advance(harness, 1.0);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Error, "a deadline lands in Error");
    expect(status.outcome == appfw::NetOutcome::Timeout,
           "verbatim: the deadline's outcome is Timeout");
    expect(status.error_kind == NetErrorKind::Timeout, "the error kind is Timeout");
    expect(harness.client.error_of(ticket) != nullptr &&
               harness.client.error_of(ticket)->kind == NetErrorKind::Timeout,
           "the delivered error is a Timeout");
    expect(harness.client.diagnostics().timed_out == 1, "the timeout is counted once");
}

// The per-attempt rule: a redirect restarts the deadline, so a chain that is slow per hop but fast
// overall does not time out - which is what section 3 item 3's "per attempt" means.
void a_redirect_restarts_the_deadline() {
    Harness harness;
    harness.fake.push_response(redirect_to(302, "https://api.example/second"), 600);
    harness.fake.push_response(response_with(200, "ok"), 600);
    const NetTicket ticket = submit(harness, request_for("https://api.example/first"), 1000);
    pump(harness);

    // 1.2 s in total, but no single attempt reached 1 s.
    advance(harness, 0.7);
    advance(harness, 0.7);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Done,
           "each hop gets its own deadline, so the chain completes");
    expect(status.outcome == appfw::NetOutcome::Ok, "the final hop is the Ok outcome");
    expect(status.attempts == 2, "two hops means two attempts");
}

// Section 3 item 4, and the verbatim acceptance assertion for cancellation.
void cancelling_before_completion_yields_cancelled_exactly_once() {
    Harness harness;
    harness.fake.push_response(response_with(200, "body"), 500);
    int deliveries = 0;
    NetResult delivered = std::unexpected(NetError{NetErrorKind::Other, "nothing arrived", 0});
    auto ticket = harness.client.submit(request_for("https://api.example/slow"), {},
                                        [&deliveries, &delivered](NetResult result) {
                                            ++deliveries;
                                            delivered = std::move(result);
                                        });
    expect(ticket.has_value(), "the fixture submits");
    pump(harness);

    expect(harness.client.cancel(*ticket), "cancelling a loading request is accepted");
    expect(!harness.client.cancel(*ticket), "a second cancel is a no-op, not a second delivery");
    expect(deliveries == 1, "verbatim: cancelling before completion delivers exactly once");
    expect(!delivered.has_value(), "a cancelled request delivers no body");
    expect(delivered.error().kind == NetErrorKind::Cancelled,
           "verbatim: the outcome of a cancel is Cancelled");
    const auto status = harness.client.status_of(*ticket);
    expect(status.outcome == appfw::NetOutcome::Cancelled, "the status records Cancelled");
    expect(harness.client.diagnostics().cancelled == 1, "the cancellation is counted once");
}

// Section 3 item 4 again, from the caller's token rather than the explicit `cancel`: a screen that
// unmounts cancels its token, and the frame-boundary `tick()` resolves what the back end has not.
void a_cancelled_token_resolves_on_the_next_tick() {
    Harness harness;
    harness.fake.push_response(response_with(200, "body"), 500);
    runtime::CancellationSource source;
    auto ticket = harness.client.submit(request_for("https://api.example/x"), source.token(), {});
    expect(ticket.has_value(), "the fixture submits");
    pump(harness);
    expect(harness.client.status_of(*ticket).state == appfw::NetRequestState::Loading,
           "the request is loading before the token is cancelled");

    source.cancel();
    harness.client.tick();
    const auto status = harness.client.status_of(*ticket);
    expect(status.outcome == appfw::NetOutcome::Cancelled,
           "a cancelled token resolves as Cancelled on the next tick");
    expect(harness.fake.cancelled_attempts() == 1,
           "the back end observed the cancellation token and released the attempt");
}

// Section 3 item 4: "cancelling after completion does nothing."
void cancelling_after_completion_does_nothing() {
    Harness harness;
    harness.fake.push_response(response_with(200, "body"));
    const NetTicket ticket = submit(harness, request_for("https://api.example/x"));
    pump(harness);
    expect(harness.client.status_of(ticket).state == appfw::NetRequestState::Done,
           "the request completed");
    expect(!harness.client.cancel(ticket), "cancelling a completed request is refused");
    expect(harness.client.status_of(ticket).outcome == appfw::NetOutcome::Ok,
           "the completed outcome is unchanged by the refused cancel");
}

// A stale transport completion must not resurrect a cancelled request or leak its body. This is the
// "exactly once" rule under the race that makes it matter: the back end answers after the cancel.
void a_late_completion_is_dropped_and_counted() {
    Harness harness;
    harness.fake.push_response(response_with(200, "a body nobody may see"), 800);
    int deliveries = 0;
    auto ticket = harness.client.submit(request_for("https://api.example/x"), {},
                                        [&deliveries](NetResult) { ++deliveries; });
    expect(ticket.has_value(), "the fixture submits");
    pump(harness);

    harness.client.cancel(*ticket);
    advance(harness, 1.0); // the fake now delivers the body it was going to deliver
    expect(deliveries == 1, "the caller heard exactly once");
    expect(harness.client.diagnostics().stale_completions >= 1,
           "the late body was dropped and counted, not delivered");
    expect(harness.client.status_of(*ticket).outcome == appfw::NetOutcome::Cancelled,
           "the late body did not overwrite the cancellation");
}

// Section 3 item 6: "when the platform reports no connectivity the result is `offline`, not
// `connect`". The two are different kinds, which is the whole point.
void offline_is_distinct_from_connect() {
    Harness offline;
    offline.fake.set_offline(true);
    offline.fake.push_response(response_with(200, "unreachable"));
    const NetTicket offline_ticket = submit(offline, request_for("https://api.example/x"));
    pump(offline);
    const auto offline_status = offline.client.status_of(offline_ticket);
    expect(offline_status.outcome == appfw::NetOutcome::Transport,
           "offline is a transport failure, not a response");
    expect(offline_status.error_kind == NetErrorKind::Offline,
           "verbatim: a platform that reports no connectivity yields offline");

    Harness down;
    down.fake.push_error(NetErrorKind::Connect, "connection refused", 0, 111);
    const NetTicket down_ticket = submit(down, request_for("https://api.example/y"));
    pump(down);
    const auto down_status = down.client.status_of(down_ticket);
    expect(down_status.error_kind == NetErrorKind::Connect,
           "a refused connection is connect, not offline");
    expect(down_status.error_kind != offline_status.error_kind,
           "offline and connect are distinct outcomes, never collapsed");
}

// A transport failure is a value with its kind intact, and it lands in Error - not in Done, which
// is where a status lands.
void a_transport_failure_is_not_a_status() {
    Harness harness;
    harness.fake.push_error(NetErrorKind::Dns, "name resolution failed", 0);
    const NetTicket ticket = submit(harness, request_for("https://missing.example/x"));
    pump(harness);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Error, "a DNS failure lands in Error");
    expect(status.outcome == appfw::NetOutcome::Transport,
           "a DNS failure is the Transport outcome");
    expect(status.error_kind == NetErrorKind::Dns, "the DNS kind survives the round trip");
    expect(status.status == 0, "no HTTP status exists for a request that got no response");
    expect(harness.client.response_of(ticket) == nullptr,
           "a failed request has no response to read");
}

// Section 3 item 5: the cap is a named `too_large`, not a truncated body.
void a_body_over_the_cap_is_too_large() {
    Harness harness;
    harness.fake.push_response(response_with(200, std::string(64, 'x')));
    NetRequest request = request_for("https://api.example/big");
    request.max_response_bytes = 16;
    harness.fake.set_available(true);
    const NetTicket ticket = submit(harness, request);
    pump(harness);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Error, "an over-cap body is an error");
    expect(status.outcome == appfw::NetOutcome::Limit, "too_large is the Limit outcome");
    expect(status.error_kind == NetErrorKind::TooLarge, "verbatim: the kind is too_large");
    expect(harness.client.response_of(ticket) == nullptr, "the data read so far is discarded");
}

// Section 3 item 2: the eleventh redirect is `too_many_redirects`.
void a_redirect_loop_is_too_many_redirects() {
    Harness harness;
    for (int hop = 0; hop < 20; ++hop) {
        harness.fake.push_response(redirect_to(302, "https://api.example/loop"));
    }
    const NetTicket ticket = submit(harness, request_for("https://api.example/loop"));
    pump(harness);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Error, "a redirect loop ends in Error");
    expect(status.outcome == appfw::NetOutcome::Limit, "a redirect loop is the Limit outcome");
    expect(status.error_kind == NetErrorKind::TooManyRedirects,
           "verbatim: the eleventh redirect is too_many_redirects");
    expect(status.attempts == appfw::kMaxRedirects + 1,
           "the limit is counted in requests, so it stops after ten redirects");
    expect(harness.fake.received().size() == appfw::kMaxRedirects + 1,
           "exactly eleven requests reached the back end, and the twelfth never did");
}

// A back end that is not present in the build yields `unsupported` before any record exists.
void an_unavailable_back_end_is_unsupported() {
    Harness harness;
    harness.fake.set_available(false);
    auto ticket = harness.client.submit(request_for("https://api.example/x"), {}, {});
    expect(!ticket.has_value(), "an unavailable back end refuses the request");
    expect(ticket.error().kind == NetErrorKind::Unsupported,
           "verbatim: an unavailable back end yields unsupported");
    expect(harness.fake.received().empty(), "nothing reached the back end");
}

// A request that never resolves at all must still be finite under the client's own machinery, and a
// destroyed client must not deliver into freed state.
void a_destroyed_client_delivers_nothing() {
    runtime::ManualClock clock;
    runtime::CompletionQueue completions{64};
    runtime::TimerQueue timers{8};
    runtime::ManualIoExecutor executor{8};
    FakeNet fake{completions, clock};
    fake.push_response(response_with(200, "late"), 1000);
    int deliveries = 0;
    {
        NetClient client(NetClientConfig{}, fake, executor, completions, timers, clock);
        auto ticket = client.submit(request_for("https://api.example/x"), {},
                                    [&deliveries](NetResult) { ++deliveries; });
        expect(ticket.has_value(), "the fixture submits");
        executor.run_one();
        executor.run_completion_one();
        clock.advance(2.0);
        fake.deliver_due();
    }
    completions.run(64);
    expect(deliveries == 0, "a client destroyed mid-flight delivers nothing");
}

} // namespace

int run_error_tests() {
    a_timeout_is_a_named_outcome();
    a_redirect_restarts_the_deadline();
    cancelling_before_completion_yields_cancelled_exactly_once();
    a_cancelled_token_resolves_on_the_next_tick();
    cancelling_after_completion_does_nothing();
    a_late_completion_is_dropped_and_counted();
    offline_is_distinct_from_connect();
    a_transport_failure_is_not_a_status();
    a_body_over_the_cap_is_too_large();
    a_redirect_loop_is_too_many_redirects();
    an_unavailable_back_end_is_unsupported();
    a_destroyed_client_delivers_nothing();
    return g_failures;
}

} // namespace net_test
