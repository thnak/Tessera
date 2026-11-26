// SPDX-License-Identifier: MIT
//
// `HttpService` (ADR-075 section 1): the interface apps resolve, with `NetClient` as its default
// implementation. The group proves the three things the ADR leans on: the client is reachable as
// the service through the registry, a request through the interface is the same request the client
// makes, and a developer's own implementation can wrap the default (here, adding an auth header)
// and still get the module's behavior underneath.

#include "appfw_net_test_support.hpp"
#include "tessera/appfw/http_service.hpp"
#include "tessera/appfw/service_context.hpp"

#include <optional>
#include <string>
#include <utility>

namespace net_test {

int run_http_service_tests();

namespace {

// A developer-written service: adds a header, then forwards to the inner one. It owns no network
// logic, which is the point.
class AuthingService final : public appfw::HttpService {
public:
    explicit AuthingService(appfw::HttpService& inner)
        : inner_(inner) {}

    runtime::Task<NetResult> send(NetRequest request, runtime::CancellationToken token) override {
        set_header(request.headers, "Authorization", "Bearer token");
        return inner_.send(std::move(request), std::move(token));
    }
    std::expected<NetTicket, NetError> submit(NetRequest request, runtime::CancellationToken token,
                                              Callback on_done) override {
        set_header(request.headers, "Authorization", "Bearer token");
        return inner_.submit(std::move(request), std::move(token), std::move(on_done));
    }
    bool cancel(NetTicket ticket) override {
        ++cancels;
        return inner_.cancel(ticket);
    }

    int cancels = 0;

private:
    appfw::HttpService& inner_;
};

void the_client_is_the_registered_service() {
    Harness harness;
    runtime::ManualClock unused;
    (void)unused;
    appfw::ServiceContext context;
    expect(context.provide<appfw::HttpService>(harness.client).has_value(),
           "NetClient registers as tessera.http");
    expect(context.freeze().has_value(), "the registry freezes");
    expect(&context.get<appfw::HttpService>() == static_cast<appfw::HttpService*>(&harness.client),
           "the registry hands back the client through the interface");
    expect(appfw::HttpService::kServiceId == appfw::service_id("tessera.http"),
           "verbatim: the service id is the hash of tessera.http");
}

void a_request_through_the_interface_reaches_the_back_end() {
    Harness harness;
    harness.fake.push_response(response_with(200, "hello"));
    appfw::HttpService& service = harness.client;

    std::optional<NetResult> result;
    auto ticket = service.submit(request_for("https://api.example/x"), {},
                                 [&result](NetResult value) { result = std::move(value); });
    expect(ticket.has_value(), "the interface accepts a valid request");
    expect(!result.has_value(), "nothing is delivered before the stages run");
    pump(harness);
    expect(result.has_value() && result->has_value() && (*result)->status == 200 &&
               (*result)->body == "hello",
           "the callback receives the scripted response");
    expect(harness.fake.received().size() == 1, "exactly one request reached the back end");
}

void a_refused_request_never_calls_back() {
    Harness harness;
    appfw::HttpService& service = harness.client;
    bool called = false;
    auto ticket =
        service.submit(request_for("not a url"), {}, [&called](NetResult) { called = true; });
    pump(harness);
    expect(!ticket.has_value() && ticket.error().kind == NetErrorKind::BadUrl,
           "a malformed URL is refused as bad_url through the interface");
    expect(!called, "a refused request never invokes on_done");
}

void a_developer_service_wraps_the_default() {
    Harness harness;
    harness.fake.push_response(response_with(204));
    AuthingService wrapped(harness.client);
    appfw::HttpService& service = wrapped;

    std::optional<NetResult> result;
    auto ticket = service.submit(request_for("https://api.example/me"), {},
                                 [&result](NetResult value) { result = std::move(value); });
    pump(harness);
    expect(ticket.has_value() && result.has_value() && result->has_value(),
           "the wrapped request completes");
    expect(!harness.fake.received().empty() &&
               find_header(harness.fake.received().front().headers, "authorization") != nullptr &&
               *find_header(harness.fake.received().front().headers, "authorization") ==
                   "Bearer token",
           "the developer's header reached the transport");

    expect(!service.cancel(*ticket),
           "cancelling a finished request through the wrapper is a no-op");
    expect(wrapped.cancels == 1, "the wrapper saw the cancel");
}

void the_default_still_enforces_its_rules_under_a_wrapper() {
    // A wrapper does not bypass the module: a cleartext request is still refused by NetClient.
    Harness harness;
    AuthingService wrapped(harness.client);
    appfw::HttpService& service = wrapped;
    auto ticket = service.submit(request_for("http://api.example/x"), {}, {});
    expect(!ticket.has_value() && ticket.error().kind == NetErrorKind::Unsupported,
           "cleartext stays refused when the default sits under a developer's service");
}

} // namespace

int run_http_service_tests() {
    the_client_is_the_registered_service();
    a_request_through_the_interface_reaches_the_back_end();
    a_refused_request_never_calls_back();
    a_developer_service_wraps_the_default();
    the_default_still_enforces_its_rules_under_a_wrapper();
    return g_failures;
}

} // namespace net_test
