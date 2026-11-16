// SPDX-License-Identifier: MIT
//
// The `net` per-request state machine (docs/specs/networking-v1.md sections 2 and 3; issue #197).
// No client, no transport, no clock: the transitions are the spec's contract, so they are asserted
// directly. The acceptance word this group owns is **"idle/loading/done/error"** and the rule that
// a 4xx/5xx response is `Done`, never `Error`.

#include "appfw_net_test_support.hpp"
#include "tessera/appfw/net_state.hpp"

#include <string_view>

namespace net_test {

using appfw::NetOutcome;
using appfw::NetStateMachine;

int run_state_tests();

namespace {

// A fresh machine starts Idle and has attempted nothing.
void idle_is_a_state() {
    const NetStateMachine machine;
    expect(machine.state() == appfw::NetRequestState::Idle, "a new request is Idle");
    expect(machine.state() == appfw::NetRequestState::Idle && !machine.finished(),
           "Idle is not terminal: a request may still be dispatched");
    expect(machine.attempts() == 0, "an Idle request has attempted nothing");
    expect(machine.state() == appfw::NetRequestState::Idle &&
               appfw::net_request_state_name(machine.state()) == std::string_view{"idle"},
           "the state name is the spec's word");
}

void dispatch_moves_to_loading() {
    NetStateMachine machine;
    expect(machine.begin_attempt(), "dispatch from Idle is accepted");
    expect(machine.state() == appfw::NetRequestState::Loading,
           "dispatch moves the request to Loading");
    expect(machine.attempts() == 1, "dispatch counts one attempt");
    expect(machine.in_flight(), "Loading is in flight");
}

void a_response_is_done_whatever_its_status() {
    {
        NetStateMachine machine;
        machine.begin_attempt();
        expect(machine.accept(200), "a 200 resolves the request");
        expect(machine.state() == appfw::NetRequestState::Done, "a 200 lands in Done");
        expect(machine.outcome() == NetOutcome::Ok, "a 200 is the Ok outcome");
        expect(machine.status() == 200, "the status is the response's");
    }
    {
        // Section 2: "A Response with a 4xx or 5xx status is not an error at this layer."
        NetStateMachine machine;
        machine.begin_attempt();
        expect(machine.accept(503), "a 503 resolves the request, it does not fail it");
        expect(machine.state() == appfw::NetRequestState::Done,
               "a 5xx is Done, not Error (section 2: a 4xx/5xx is not an error here)");
        expect(machine.outcome() == NetOutcome::HttpStatus, "a 5xx is the HttpStatus outcome");
        expect(!machine.error_kind().has_value(), "no error kind is recorded for a 5xx");
    }
    {
        NetStateMachine machine;
        machine.begin_attempt();
        machine.accept(404);
        expect(machine.state() == appfw::NetRequestState::Done, "a 4xx is Done as well");
        expect(machine.outcome() == NetOutcome::HttpStatus, "a 4xx is the HttpStatus outcome");
    }
}

void a_transport_error_is_distinct_from_a_status() {
    NetStateMachine machine;
    machine.begin_attempt();
    expect(machine.fail(NetError{NetErrorKind::Connect, "connection refused", 111}),
           "a transport failure resolves the request");
    expect(machine.state() == appfw::NetRequestState::Error, "a transport failure lands in Error");
    expect(machine.outcome() == NetOutcome::Transport,
           "a connect failure is the Transport outcome");
    expect(machine.error_kind() == NetErrorKind::Connect, "the kind is the transport's own");
}

// The three outcomes the acceptance names must be three values, not one "failed".
void the_three_named_outcomes_are_distinct() {
    NetStateMachine timed_out;
    timed_out.begin_attempt();
    timed_out.timed_out();
    expect(timed_out.outcome() == NetOutcome::Timeout, "a deadline is Timeout");

    NetStateMachine cancelled;
    cancelled.begin_attempt();
    cancelled.cancelled();
    expect(cancelled.outcome() == NetOutcome::Cancelled, "a cancellation is Cancelled");

    NetStateMachine failed;
    failed.begin_attempt();
    failed.fail(NetError{NetErrorKind::Tls, "certificate", 0});
    expect(failed.outcome() == NetOutcome::Transport, "a TLS failure is Transport");

    expect(NetOutcome::Timeout != NetOutcome::Cancelled &&
               NetOutcome::Cancelled != NetOutcome::Transport &&
               NetOutcome::Timeout != NetOutcome::Transport,
           "timeout, cancelled and a transport failure are three distinct outcomes");
}

// Cancellation and timeout both land in Error, which is why the outcome - not the state - is what a
// caller branches on.
void cancellation_and_timeout_are_both_terminal_errors() {
    NetStateMachine timed_out;
    timed_out.begin_attempt();
    timed_out.timed_out();
    expect(timed_out.state() == appfw::NetRequestState::Error, "a timeout is Error, not Done");
    expect(timed_out.error_kind() == NetErrorKind::Timeout, "a timeout records the Timeout kind");

    NetStateMachine cancelled;
    cancelled.begin_attempt();
    cancelled.cancelled();
    expect(cancelled.state() == appfw::NetRequestState::Error, "a cancellation is Error, not Done");
    expect(cancelled.error_kind() == NetErrorKind::Cancelled,
           "a cancellation records the Cancelled kind");
}

// Terminal is terminal: every later transition is refused, which is what makes "exactly once"
// structural. A stale transport callback therefore cannot overwrite a cancellation.
void a_terminal_machine_refuses_everything() {
    NetStateMachine machine;
    machine.begin_attempt();
    machine.cancelled();
    expect(!machine.begin_attempt(), "a terminal request cannot start another attempt");
    expect(!machine.accept(200), "a response after a cancel is refused");
    expect(!machine.fail(NetError{NetErrorKind::Connect, "", 0}),
           "a failure after a cancel is refused");
    expect(!machine.timed_out(), "a second terminal transition is refused");
    expect(machine.outcome() == NetOutcome::Cancelled,
           "the first terminal outcome survives every later attempt");
    expect(machine.attempts() == 1, "a refused attempt does not increment the count");
}

// A followed redirect is a new hop of the same request: Loading -> Loading, and `attempts` counts
// requests, which is the number the section 3 item 2 limit is measured in.
void a_redirect_is_a_new_attempt_of_the_same_request() {
    NetStateMachine machine;
    machine.begin_attempt();
    machine.accept(302);
    expect(machine.finished(), "the 302 itself resolves the request");
    // The client's redirect path starts the next hop before the response is accepted, so the
    // machine sees a second `begin_attempt` while still Loading. Reproduce that order.
    NetStateMachine hopping;
    hopping.begin_attempt();
    expect(hopping.begin_attempt(), "a second hop while Loading is accepted");
    expect(hopping.state() == appfw::NetRequestState::Loading,
           "a redirect keeps the request in Loading: it is not finished");
    expect(hopping.attempts() == 2, "a redirect counts a second attempt");
}

void the_kind_to_outcome_mapping_is_total() {
    expect(appfw::outcome_of(NetErrorKind::Cancelled) == NetOutcome::Cancelled, "cancelled maps");
    expect(appfw::outcome_of(NetErrorKind::Timeout) == NetOutcome::Timeout, "timeout maps");
    expect(appfw::outcome_of(NetErrorKind::Offline) == NetOutcome::Transport, "offline maps");
    expect(appfw::outcome_of(NetErrorKind::Dns) == NetOutcome::Transport, "dns maps");
    expect(appfw::outcome_of(NetErrorKind::Connect) == NetOutcome::Transport, "connect maps");
    expect(appfw::outcome_of(NetErrorKind::Tls) == NetOutcome::Transport, "tls maps");
    expect(appfw::outcome_of(NetErrorKind::TooLarge) == NetOutcome::Limit, "too_large maps");
    expect(appfw::outcome_of(NetErrorKind::TooManyRedirects) == NetOutcome::Limit,
           "too_many_redirects maps");
    expect(appfw::outcome_of(NetErrorKind::BadUrl) == NetOutcome::Request, "bad_url maps");
    expect(appfw::outcome_of(NetErrorKind::Unsupported) == NetOutcome::Request, "unsupported maps");
    expect(appfw::outcome_of(NetErrorKind::Other) == NetOutcome::Request, "other maps");
}

} // namespace

int run_state_tests() {
    idle_is_a_state();
    dispatch_moves_to_loading();
    a_response_is_done_whatever_its_status();
    a_transport_error_is_distinct_from_a_status();
    the_three_named_outcomes_are_distinct();
    cancellation_and_timeout_are_both_terminal_errors();
    a_terminal_machine_refuses_everything();
    a_redirect_is_a_new_attempt_of_the_same_request();
    the_kind_to_outcome_mapping_is_total();
    return g_failures;
}

} // namespace net_test
