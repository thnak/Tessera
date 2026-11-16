// SPDX-License-Identifier: MIT
//
// Fixtures for the `net` contract suite (docs/specs/networking-v1.md section 5; issue #197).
//
// Determinism is structural, not a promise: time is a `ManualClock`, the I/O stage is a
// `ManualIoExecutor`, the deadline is a `TimerQueue` fired by hand, the network is `FakeNet`, and
// the delivery stage is a `CompletionQueue` the test drains. No test in this suite opens a socket,
// reads a wall clock, sleeps, starts a thread or touches the file system - and because every one of
// those is a hand-driven FIFO, a test asserts *what happened at each stage* instead of waiting for
// it.
//
// The pump deliberately separates the stages the way a frame does:
//
//   executor work -> executor completion -> back-end delivery -> timers -> completions
//
// which is `RuntimeLoop::tick()`'s order (drain completions, then fire timers), so a bug that only
// appears when the order changes is visible here.

#pragma once

#include "tessera/appfw/net_client.hpp"
#include "tessera/appfw/net_fake.hpp"
#include "tessera/appfw/net_types.hpp"
#include "tessera/appfw/net_url.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/manual_io_executor.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace net_test {

namespace appfw = tessera::appfw;
namespace runtime = tessera::runtime;

using appfw::FakeNet;
using appfw::find_header;
using appfw::has_header;
using appfw::NetClient;
using appfw::NetClientConfig;
using appfw::NetError;
using appfw::NetErrorKind;
using appfw::NetHeaders;
using appfw::NetRequest;
using appfw::NetResponse;
using appfw::NetResult;
using appfw::NetTicket;
using appfw::redacted_url;
using appfw::RedirectPolicy;
using appfw::remove_header;
using appfw::set_header;

inline int g_failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_net: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// The whole deterministic stack. Declaration order is dependency order: the clock and the queues
// are destroyed last because the client, the fake and the timers all borrow them.
struct Harness {
    explicit Harness(NetClientConfig cfg = {})
        : config(std::move(cfg)),
          client(config, fake, executor, completions, timers, clock) {}

    runtime::ManualClock clock;
    runtime::CompletionQueue completions{256};
    runtime::TimerQueue timers{16};
    runtime::ManualIoExecutor executor{32};
    FakeNet fake{completions, clock};
    NetClientConfig config;
    NetClient client;
};

// Advances the clock and then runs everything that became ready, so "one second later" is one call.
inline std::size_t advance(Harness& harness, double seconds, std::size_t budget = 64);

// Runs every stage until nothing is ready. Returns how many rounds made progress, which is what a
// test asserts when it needs to prove a request *stopped* making progress (a parked request returns
// the same number twice).
inline std::size_t pump(Harness& harness, std::size_t budget = 64) {
    std::size_t rounds = 0;
    for (std::size_t index = 0; index < budget; ++index) {
        bool progress = false;
        progress = harness.executor.run_one() || progress;
        progress = harness.executor.run_completion_one() || progress;
        progress = harness.fake.deliver_due() > 0 || progress;
        progress = harness.timers.fire_due(harness.clock.now_seconds()) > 0 || progress;
        progress = harness.completions.run(64) > 0 || progress;
        if (!progress) {
            return rounds;
        }
        ++rounds;
    }
    return rounds;
}

inline std::size_t advance(Harness& harness, double seconds, std::size_t budget) {
    harness.clock.advance(seconds);
    return pump(harness, budget);
}

// A request that a back end can serve. `https` because section 3 item 8 refuses cleartext by
// default.
inline NetRequest request_for(std::string url) {
    NetRequest request;
    request.url = std::move(url);
    return request;
}

inline NetResponse response_with(int status, std::string body = {}) {
    NetResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

inline NetResponse redirect_to(int status, std::string location) {
    NetResponse response;
    response.status = status;
    response.headers.emplace_back("Location", std::move(location));
    return response;
}

// Drives the `send` coroutine to completion and hands the `expected` back through `out`.
//
// A `runtime::Task<T>`'s value is readable only by awaiting it, so the suite cannot call `send`,
// pump, and then inspect the task - it has to await from inside another coroutine. This is that
// coroutine: context-first, because `Task`'s promise consumes the context there
// (docs/design/services.md section 1.1), and with the result written into a caller-owned slot so
// the assertion reads a plain value. It exercises the real path, including the awaiter's suspension
// and its queue-driven resume.
inline runtime::Task<void> await_send(runtime::TaskContext, NetClient& client, NetRequest request,
                                      runtime::CancellationToken token, NetResult& out) {
    out = co_await client.send(std::move(request), std::move(token));
    co_return;
}

// Starts `await_send` and pumps until it is done (or the budget runs out). Returns whether the task
// finished without an exception.
inline bool run_send(Harness& harness, NetResult& out, NetRequest request,
                     runtime::CancellationToken token = {}, int budget = 64) {
    runtime::TaskContext context;
    context.queue = &harness.completions;
    auto task = await_send(context, harness.client, std::move(request), std::move(token), out);
    if (!task.start()) {
        return false;
    }
    for (int round = 0; round < budget && !task.done(); ++round) {
        pump(harness);
        harness.completions.run(64);
    }
    return task.done() && task.exception() == nullptr;
}

// Submits with an empty callback and asserts acceptance, so a test that is not about a refusal does
// not have to unwrap the `expected` at every call site.
inline NetTicket submit(Harness& harness, NetRequest request, std::uint32_t timeout_ms = 30000) {
    request.timeout_ms = timeout_ms;
    auto ticket = harness.client.submit(std::move(request), {}, {});
    if (!ticket.has_value()) {
        std::cerr << "appfw_net: fixture failed: submit refused: " << ticket.error().message
                  << "\n";
        std::exit(1);
    }
    return *ticket;
}

} // namespace net_test
