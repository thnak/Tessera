// SPDX-License-Identifier: MIT
//
// The net contract against the real OS back end (WinHTTP on Windows, libcurl on Linux), over an
// in-process loopback server (issues #835 and #836, docs/specs/networking-v1.md section 5). Opt-in
// lane: `TESSERA_NET_LOOPBACK_TESTS=ON`. No test touches the external network: every request goes
// to 127.0.0.1. One file for both back ends, because the contract is the same; only the transport
// type and its name differ.
//
// Unlike the `FakeNet` suite this one is not deterministic by construction: the server is a real
// socket on its own thread and the back end is the OS stack. So time is bounded, not manual. Every
// wait has a deadline and a miss is a named failure, and each timing the suite asserts has a wide
// margin (a delay of 600 ms against a 150 ms deadline), because a loaded CI machine must not turn a
// correct back end red.
//
// The stack is the production one: a `WorkerIoExecutor` starts each attempt, `SteadyClock` and a
// `TimerQueue` hold the deadline, and the test drains the `CompletionQueue` the way a frame does.

#include "support/loopback_server.hpp"
#include "tessera/appfw/net_client.hpp"
#if defined(TESSERA_NET_HAS_WINHTTP)
#include "tessera/appfw/net_winhttp.hpp"
#elif defined(TESSERA_NET_HAS_CURL)
#include "tessera/appfw/net_curl.hpp"
#else
#error "the loopback lane needs a real back end: WinHTTP or libcurl"
#endif
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/runtime/timer_queue.hpp"
#include "tessera/runtime/worker_io_executor.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace appfw = tessera::appfw;
namespace runtime = tessera::runtime;
using tessera::net_test::LoopbackResponse;
using tessera::net_test::LoopbackServer;
using Clock = std::chrono::steady_clock;

#if defined(TESSERA_NET_HAS_WINHTTP)
using RealTransport = appfw::WinHttpTransport;
constexpr std::string_view kBackendName = "winhttp";
#else
using RealTransport = appfw::CurlTransport;
constexpr std::string_view kBackendName = "curl";
#endif

int failures = 0;

bool check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
    return condition;
}

// The production stack. Declaration order is dependency order, so the client goes first and the
// transport (which waits for the OS to release every handle) before the executor and the queue.
struct Stack {
    Stack()
        : Stack(make_config()) {}
    explicit Stack(appfw::NetClientConfig config)
        : client(std::move(config), transport, executor, completions, timers, clock) {}

    static appfw::NetClientConfig make_config() {
        appfw::NetClientConfig config;
        config.allow_cleartext = true; // loopback is plain http
        return config;
    }

    runtime::CompletionQueue completions{256};
    runtime::SteadyClock clock;
    runtime::TimerQueue timers{16};
    runtime::WorkerIoExecutor executor{completions};
    RealTransport transport{completions};
    appfw::NetClient client;
};

// Drains the queue, fires due deadlines and ticks the client until `done` or the budget runs out.
template<typename Done> bool pump_until(Stack& stack, Done done, int budget_ms = 4000) {
    const auto end = Clock::now() + std::chrono::milliseconds(budget_ms);
    while (Clock::now() < end) {
        (void)stack.completions.run(64);
        (void)stack.timers.fire_due(stack.clock.now_seconds());
        stack.client.tick();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}

// Submits and waits for the one result.
std::optional<appfw::NetResult> fetch(Stack& stack, appfw::NetRequest request,
                                      runtime::CancellationToken token = {}) {
    std::optional<appfw::NetResult> result;
    auto ticket =
        stack.client.submit(std::move(request), std::move(token),
                            [&result](appfw::NetResult value) { result = std::move(value); });
    if (!ticket) {
        return appfw::NetResult{std::unexpected(ticket.error())};
    }
    (void)pump_until(stack, [&] { return result.has_value(); });
    return result;
}

appfw::NetRequest get(const LoopbackServer& server, std::string path = "/") {
    appfw::NetRequest request;
    request.url = server.base_url() + path;
    return request;
}

bool is_error(const std::optional<appfw::NetResult>& result, appfw::NetErrorKind kind) {
    return result.has_value() && !result->has_value() && result->error().kind == kind;
}

void test_identity() {
    Stack stack;
    check(stack.transport.available(), "the back end initialised");
    check(stack.transport.name() == kBackendName, "the transport names itself");
    check(stack.client.transport_name() == kBackendName, "the client reports it");
}

void test_get_response() {
    LoopbackServer server;
    LoopbackResponse reply;
    reply.headers = {{"X-Reply", "yes"}, {"Content-Type", "text/plain"}};
    reply.body = "hello";
    check(server.start({reply}), "the server starts");
    Stack stack;
    appfw::NetRequest request = get(server, "/path?x=1");
    request.headers.emplace_back("X-Asked", "1");
    const auto result = fetch(stack, request);
    if (!check(result.has_value() && result->has_value(), "a GET resolves with a response")) {
        return;
    }
    const appfw::NetResponse& response = **result;
    check(response.status == 200, "the status is 200");
    check(response.body == "hello", "the body arrives whole");
    check(appfw::has_header(response.headers, "x-reply"), "a response header is kept");
    check(response.final_url == request.url, "final_url is the request URL");
    check(response.protocol != appfw::NetProtocol::Unknown, "the protocol is reported");
    const auto seen = server.requests();
    check(seen.size() == 1 && seen[0].method == "GET" && seen[0].target == "/path?x=1",
          "the server saw method and target");
    check(seen.size() == 1 && seen[0].header("x-asked") == "1", "the caller's header was sent");
    check(seen.size() == 1 && seen[0].header("user-agent").rfind("Tessera/", 0) == 0,
          "the module's fixed User-Agent was sent, and only that");
}

void test_post_body() {
    LoopbackServer server;
    check(server.start({LoopbackResponse{}}), "the server starts");
    Stack stack;
    appfw::NetRequest request = get(server, "/submit");
    request.method = appfw::HttpMethod::Post;
    request.body = "payload-bytes";
    request.has_body = true;
    request.headers.emplace_back("Content-Type", "text/plain");
    const auto result = fetch(stack, request);
    check(result.has_value() && result->has_value(), "a POST resolves");
    const auto seen = server.requests();
    check(seen.size() == 1 && seen[0].method == "POST" && seen[0].body == "payload-bytes",
          "the body reached the server");
}

void test_error_status_is_a_response() {
    LoopbackServer server;
    LoopbackResponse reply;
    reply.status = 503;
    reply.reason = "Service Unavailable";
    check(server.start({reply}), "the server starts");
    Stack stack;
    const auto result = fetch(stack, get(server));
    check(result.has_value() && result->has_value() && (*result)->status == 503,
          "a 5xx is a response, not an error");
}

void test_redirects_belong_to_the_module() {
    LoopbackServer server;
    LoopbackResponse hop;
    hop.status = 302;
    hop.reason = "Found";
    LoopbackResponse last;
    last.body = "arrived";
    // The first hop's Location is written once the port is known.
    check(server.start({hop, last}), "the server starts");
    // Manual: the 3xx is the result and the transport made exactly one request.
    {
        Stack stack;
        appfw::NetRequest request = get(server);
        request.redirect = appfw::RedirectPolicy::Manual;
        const auto result = fetch(stack, request);
        check(result.has_value() && result->has_value() && (*result)->status == 302,
              "a manual redirect hands the 3xx back");
        check(server.request_count() == 1, "the transport followed nothing");
    }
}

void test_follow_and_cross_origin_authorization() {
    LoopbackServer target;
    LoopbackResponse done;
    done.body = "arrived";
    check(target.start({done}), "the target starts");
    LoopbackServer origin;
    LoopbackResponse hop;
    hop.status = 302;
    hop.reason = "Found";
    hop.headers = {{"Location", target.base_url() + "/landed"}};
    check(origin.start({hop}), "the origin starts");

    Stack stack;
    appfw::NetRequest request = get(origin, "/start");
    request.headers.emplace_back("Authorization", "Bearer secret");
    const auto result = fetch(stack, request);
    if (!check(result.has_value() && result->has_value(), "the redirect chain resolves")) {
        return;
    }
    check((*result)->body == "arrived", "the second hop's body is the result");
    check((*result)->final_url == target.base_url() + "/landed", "final_url is the last hop");
    const auto first = origin.requests();
    const auto second = target.requests();
    check(first.size() == 1 && first[0].header("authorization") == "Bearer secret",
          "the origin received the credential");
    check(second.size() == 1 && second[0].header("authorization").empty(),
          "a cross-origin redirect dropped Authorization");
}

void test_connect_refused_is_connect() {
    std::string url;
    {
        LoopbackServer server;
        check(server.start({LoopbackResponse{}}), "the server starts");
        url = server.base_url();
    } // the port is closed again
    Stack stack;
    appfw::NetRequest request;
    request.url = url + "/";
    const auto result = fetch(stack, request);
    check(is_error(result, appfw::NetErrorKind::Connect),
          "a closed port is connect, not dns/timeout");
}

void test_size_limit() {
    LoopbackServer server;
    LoopbackResponse reply;
    reply.body = std::string(4096, 'x');
    check(server.start({reply}), "the server starts");
    Stack stack;
    appfw::NetRequest request = get(server);
    request.max_response_bytes = 1024;
    const auto result = fetch(stack, request);
    check(is_error(result, appfw::NetErrorKind::TooLarge), "a body past the cap is too_large");
}

void test_deadline() {
    LoopbackServer server;
    LoopbackResponse reply;
    reply.delay_ms = 600;
    check(server.start({reply}), "the server starts");
    Stack stack;
    appfw::NetRequest request = get(server);
    request.timeout_ms = 150;
    const auto start = Clock::now();
    const auto result = fetch(stack, request);
    check(is_error(result, appfw::NetErrorKind::Timeout), "a slow server is a timeout");
    check(Clock::now() - start < std::chrono::milliseconds(550),
          "the deadline fired before the answer");
}

void test_cancel_releases_the_request() {
    LoopbackServer server;
    LoopbackResponse reply;
    reply.delay_ms = 800;
    check(server.start({reply}), "the server starts");
    Stack stack;
    runtime::CancellationSource source;
    std::optional<appfw::NetResult> result;
    auto ticket =
        stack.client.submit(get(server), source.token(),
                            [&result](appfw::NetResult value) { result = std::move(value); });
    check(ticket.has_value(), "the request is accepted");
    check(server.wait_for_requests(1), "the request reached the server");
    const auto cancelled_at = Clock::now();
    source.cancel();
    check(pump_until(stack, [&] { return result.has_value(); }), "the cancel resolved the request");
    check(is_error(result, appfw::NetErrorKind::Cancelled), "the result is cancelled");
    check(Clock::now() - cancelled_at < std::chrono::milliseconds(600),
          "it resolved without waiting for the answer");
}

void test_start_does_not_block() {
    LoopbackServer server;
    LoopbackResponse reply;
    reply.delay_ms = 400;
    check(server.start({reply}), "the server starts");
    Stack stack;
    std::optional<appfw::NetResult> result;
    const auto before = Clock::now();
    auto ticket = stack.client.submit(
        get(server), {}, [&result](appfw::NetResult value) { result = std::move(value); });
    check(Clock::now() - before < std::chrono::milliseconds(150),
          "submit returns while the request is still in flight");
    check(!result.has_value(), "no completion ran inline");
    check(ticket.has_value() && pump_until(stack, [&] { return result.has_value(); }),
          "the request still completes");
}

void test_bad_url_never_reaches_the_network() {
    Stack stack;
    appfw::NetRequest request;
    request.url = "not a url";
    const auto result = fetch(stack, request);
    check(result.has_value() && !result->has_value(), "a bad URL is an error");
}

} // namespace

int main() {
    test_identity();
    test_get_response();
    test_post_body();
    test_error_status_is_a_response();
    test_redirects_belong_to_the_module();
    test_follow_and_cross_origin_authorization();
    test_connect_refused_is_connect();
    test_size_limit();
    test_deadline();
    test_cancel_releases_the_request();
    test_start_does_not_block();
    test_bad_url_never_reaches_the_network();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
