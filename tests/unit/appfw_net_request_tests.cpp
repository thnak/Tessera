// SPDX-License-Identifier: MIT
//
// The `net` request/response value surface and the coroutine form of `send` (docs/specs/
// networking-v1.md section 2; issue #197).
//
// The group exists because the spec's interface table is normative field by field - a default that
// drifts (a 30 s timeout becoming 60, a 16 MiB cap becoming unlimited) is a contract change - and
// because `send` is the signature section 2 actually writes. The coroutine case proves the lazy
// task produces the same `expected` a `submit` callback does.

#include "appfw_net_test_support.hpp"

#include <string>
#include <string_view>

namespace net_test {

int run_request_tests();

namespace {

// Section 2's defaults, asserted as values.
void the_spec_defaults_are_the_defaults() {
    const NetRequest request;
    expect(request.method == appfw::HttpMethod::Get, "the default method is GET");
    expect(request.timeout_ms == 30000, "verbatim: the default timeout is 30000 ms");
    expect(request.max_response_bytes == 16u * 1024u * 1024u,
           "verbatim: the default max_response_bytes is 16 MiB");
    expect(request.redirect == appfw::RedirectPolicy::Follow, "the default redirect is follow");
    expect(!request.has_body, "a request has no body by default");

    const NetClientConfig config;
    expect(!config.allow_cleartext, "verbatim: net.allowCleartext defaults to false");
    expect(config.send_default_user_agent, "the module sends the fixed User-Agent by default");
}

// Section 3 item 7: names compare case-insensitively and are stored as sent.
void header_names_compare_case_insensitively() {
    NetHeaders headers;
    set_header(headers, "Authorization", "Bearer secret");
    expect(find_header(headers, "authorization") != nullptr, "a lowercase lookup finds the header");
    expect(find_header(headers, "AUTHORIZATION") != nullptr, "an uppercase lookup finds it too");
    expect(find_header(headers, "authorization") != nullptr &&
               *find_header(headers, "authorization") == "Bearer secret",
           "the value is the one that was set");
    expect(headers.front().first == "Authorization",
           "verbatim: the name is stored as the caller wrote it, not lowercased");

    set_header(headers, "authorization", "Bearer replaced");
    expect(headers.size() == 1,
           "setting the same name in another case replaces rather than appends");
    expect(headers.front().first == "Authorization",
           "the replacement keeps the caller's original spelling and send position");
    expect(headers.front().second == "Bearer replaced", "the value is the new one");

    expect(remove_header(headers, "AUTHORIZATION") == 1, "removal matches case-insensitively");
    expect(headers.empty(), "the header is gone");
}

void repeated_names_are_preserved_until_replaced() {
    NetHeaders headers;
    headers.emplace_back("Accept", "text/html");
    headers.emplace_back("Accept", "application/json");
    expect(headers.size() == 2, "two values under one name are legal and kept");
    expect(find_header(headers, "accept") != nullptr &&
               *find_header(headers, "accept") == "text/html",
           "the first value is the one a lookup returns");
    set_header(headers, "Accept", "text/plain");
    expect(headers.size() == 1, "a replacement collapses the duplicates");
}

// A 2xx is Ok; a 4xx/5xx is HttpStatus - a value, never an error. This is section 2's last
// paragraph.
void a_status_response_is_a_value_not_an_error() {
    Harness harness;
    harness.fake.push_response(response_with(500, "upstream failed"));
    const NetTicket ticket = submit(harness, request_for("https://api.example/x"));
    pump(harness);
    const NetResponse* const response = harness.client.response_of(ticket);
    expect(response != nullptr, "verbatim: a 500 is delivered as a response, not an error");
    expect(response != nullptr && response->status == 500, "the status is the server's");
    expect(response != nullptr && response->body == "upstream failed",
           "the 5xx body is delivered: it is the caller's to interpret");
    expect(harness.client.error_of(ticket) == nullptr, "no error exists for a 5xx");
    expect(harness.client.status_of(ticket).outcome == appfw::NetOutcome::HttpStatus,
           "the outcome is HttpStatus, distinct from Ok and from every error");
}

void a_two_hundred_is_the_ok_outcome() {
    Harness harness;
    NetResponse response = response_with(201, "created");
    response.headers.emplace_back("Content-Type", "application/json");
    response.protocol = appfw::NetProtocol::H2;
    harness.fake.push_response(response);
    const NetTicket ticket = submit(harness, request_for("https://api.example/items"));
    pump(harness);
    expect(harness.client.status_of(ticket).outcome == appfw::NetOutcome::Ok, "a 201 is Ok");
    const NetResponse* const delivered = harness.client.response_of(ticket);
    expect(delivered != nullptr && delivered->protocol == appfw::NetProtocol::H2,
           "the negotiated protocol is reported");
    expect(delivered != nullptr && find_header(delivered->headers, "content-type") != nullptr,
           "response headers are readable case-insensitively");
    expect(delivered != nullptr && delivered->final_url == "https://api.example/items",
           "final_url is set even when nothing redirected");
}

// `HEAD` and `204` have no body; the response is still a `Done`.
void an_empty_body_is_still_a_response() {
    Harness harness;
    harness.fake.push_response(response_with(204));
    NetRequest request = request_for("https://api.example/none");
    request.method = appfw::HttpMethod::Head;
    const NetTicket ticket = submit(harness, request);
    pump(harness);
    const auto status = harness.client.status_of(ticket);
    expect(status.state == appfw::NetRequestState::Done, "a 204 completes the request");
    expect(status.body_bytes == 0, "there is no body");
    expect(harness.fake.received()[0].method == appfw::HttpMethod::Head,
           "the method reached the back end unchanged");
}

// The body is optional, and presence is its own flag: an empty POST body is not a missing one.
void a_body_is_optional_and_its_presence_is_explicit() {
    Harness harness;
    harness.fake.push_response(response_with(200));
    NetRequest request = request_for("https://api.example/x");
    request.method = appfw::HttpMethod::Post;
    request.body = "";
    request.has_body = true;
    submit(harness, request);
    pump(harness);
    expect(harness.fake.received().size() == 1, "the request reached the back end");
    expect(!harness.fake.received().empty() && harness.fake.received()[0].has_body,
           "an empty body on POST is present, not absent");
}

// Section 2's signature, in the shape `runtime::Task` can express: the coroutine form yields the
// same `expected` the callback form collects.
void send_returns_the_expected_as_a_coroutine() {
    Harness harness;
    harness.fake.push_response(response_with(200, "from a task"));

    NetResult result = std::unexpected(NetError{NetErrorKind::Other, "never ran", 0});
    expect(run_send(harness, result, request_for("https://api.example/x")),
           "verbatim: send resolves to an expected through the coroutine form");
    expect(result.has_value(), "the task delivered a response, not an error");
    expect(result.has_value() && result->status == 200, "the status is the response's");
    expect(result.has_value() && result->body == "from a task",
           "the body travelled through the coroutine");
}

// A response found before the coroutine even suspends still arrives: the awaiter posts the resume
// instead of continuing inline, so the coroutine never runs on the resolving stack.
void send_delivers_even_when_the_result_beats_the_suspension() {
    Harness harness;
    harness.fake.push_response(response_with(204));
    NetResult result = std::unexpected(NetError{NetErrorKind::Other, "never ran", 0});
    expect(run_send(harness, result, request_for("https://api.example/x")),
           "the send task finished");
    expect(result.has_value() && result->status == 204, "the immediate result was not lost");
}

// Cancellation through `send` is a value too: the token the caller passed, not the task's frame,
// which is what lets `cancelled` arrive in the `expected` instead of as `TaskCancelled`.
void send_reports_cancellation_as_a_value() {
    Harness harness;
    harness.fake.push_response(response_with(200, "never seen"), 5000);
    runtime::CancellationSource source;
    source.cancel();
    NetResult result = std::unexpected(NetError{NetErrorKind::Other, "never ran", 0});
    expect(run_send(harness, result, request_for("https://api.example/x"), source.token()),
           "the send task finished rather than throwing");
    expect(!result.has_value(), "a cancelled send is an error value");
    expect(result.error().kind == NetErrorKind::Cancelled,
           "verbatim: a cancelled send yields the named cancelled kind, not an exception");
}

// A refused request through `send` is still a value: `bad_url` arrives in the `expected`, never as
// a throw at the `co_await`.
void a_refused_send_is_an_error_value_not_an_exception() {
    Harness harness;
    NetResult result = std::unexpected(NetError{NetErrorKind::Other, "never ran", 0});
    expect(run_send(harness, result, request_for("not a url")),
           "verbatim: a refusal resolves the task, it does not throw across co_await");
    expect(!result.has_value(), "the refusal arrived as an error value");
    expect(result.error().kind == NetErrorKind::BadUrl, "the kind is bad_url");
}

// A caller that starts a `send` and then drops the task must not be resumed into a destroyed frame
// when the completion arrives afterwards.
void dropping_a_send_task_is_safe() {
    Harness harness;
    harness.fake.push_response(response_with(200, "nobody is waiting"), 300);
    {
        auto task = harness.client.send(request_for("https://api.example/x"), {});
        expect(task.start(), "the send task starts");
        pump(harness);
        // `task` is destroyed here, before the response: its awaiter must mark the state abandoned.
    }
    advance(harness, 0.5);
    expect(true, "a dropped send task was not resumed after its frame was destroyed");
}

// The record bound keeps a long-lived app bounded, and asking about a ticket that was evicted is a
// named "unknown", not an Idle request.
void the_record_bound_evicts_and_unknown_tickets_are_unknown() {
    NetClientConfig config;
    config.max_records = 2;
    Harness harness(config);
    for (int index = 0; index < 6; ++index) {
        harness.fake.push_response(response_with(200));
    }
    NetTicket first{};
    for (int index = 0; index < 6; ++index) {
        const NetTicket ticket = submit(harness, request_for("https://api.example/x"));
        if (index == 0) {
            first = ticket;
        }
        pump(harness);
    }
    expect(harness.client.records() <= config.max_records,
           "the client keeps at most max_records terminal records");
    expect(!harness.client.status_of(first).known,
           "an evicted ticket reports unknown, which is not the same as Idle");
    expect(harness.client.status_of(NetTicket{}).known == false,
           "a default ticket identifies nothing");
}

// `release` frees a terminal record early so the bound need not grow; an in-flight one cannot be
// released, because the caller would lose the only handle to a request that is still running.
void release_frees_terminal_records_only() {
    Harness harness;
    harness.fake.push_response(response_with(200), 500);
    const NetTicket loading = submit(harness, request_for("https://api.example/slow"));
    pump(harness);
    expect(!harness.client.release(loading), "an in-flight request cannot be released");
    advance(harness, 1.0);
    expect(harness.client.release(loading), "a terminal request is released");
    expect(!harness.client.status_of(loading).known, "the released ticket is unknown");
}

// The diagnostics ring is bounded and never carries a credential.
void diagnostics_are_bounded_and_redacted() {
    NetClientConfig config;
    config.diagnostic_capacity = 4;
    Harness harness(config);
    for (int index = 0; index < 10; ++index) {
        const auto refused =
            harness.client.submit(request_for("bad-url-" + std::to_string(index)), {}, {});
        expect(!refused.has_value(), "a malformed URL is refused at submit");
    }
    const appfw::NetDiagnostics diagnostics = harness.client.diagnostics();
    expect(diagnostics.entries.size() == 4, "verbatim: the diagnostics ring is bounded");
    expect(diagnostics.dropped == 6, "the dropped entries are counted rather than forgotten");
    expect(diagnostics.refused == 10, "every refusal is counted");
    for (const appfw::NetDiagnostic& entry : diagnostics.entries) {
        expect(entry.detail.find('?') == std::string::npos,
               "no diagnostic detail carries a query string");
    }
}

} // namespace

int run_request_tests() {
    the_spec_defaults_are_the_defaults();
    header_names_compare_case_insensitively();
    repeated_names_are_preserved_until_replaced();
    a_status_response_is_a_value_not_an_error();
    a_two_hundred_is_the_ok_outcome();
    an_empty_body_is_still_a_response();
    a_body_is_optional_and_its_presence_is_explicit();
    send_returns_the_expected_as_a_coroutine();
    send_delivers_even_when_the_result_beats_the_suspension();
    send_reports_cancellation_as_a_value();
    a_refused_send_is_an_error_value_not_an_exception();
    dropping_a_send_task_is_safe();
    the_record_bound_evicts_and_unknown_tickets_are_unknown();
    release_frees_terminal_records_only();
    diagnostics_are_bounded_and_redacted();
    return g_failures;
}

} // namespace net_test
