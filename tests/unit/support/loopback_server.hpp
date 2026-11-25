// SPDX-License-Identifier: MIT
//
// An in-process loopback HTTP server for the net contract suite (issue #882, networking-v1 section
// 5).
//
// **Why this exists.** Section 5 says every real back end runs the same contract suite "against an
// in-process loopback server in an opt-in lane (`TESSERA_NET_LOOPBACK_TESTS=ON`); no test touches
// the external network". That lane did not exist: the suite was pinned to `FakeNet`, so a WinHTTP
// or libcurl back end could only be tested by hitting a real host, which the spec forbids and CI
// cannot do.
//
// **Why a hand-written server and not a library.** The tests need three things and nothing else: a
// request the test scripted, a response the test scripted, and a port that is closed on teardown.
// An HTTP library would be a new dependency in the test path for a few dozen lines of HTTP/1.1, and
// the protocol subset the tests exercise is deliberately small (see `Response`).
//
// **What it is not.** It does not do keep-alive, chunked encoding, or TLS. A back end that depends
// on those is exercising its own platform stack, not this file; the tests that need them use a real
// host in a lane the spec calls out separately.
//
// Threading: one accept loop on its own thread. A test writes a script before it starts and reads
// what arrived after it stops, so there is one mutex around the scripts and no other shared state.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tessera::net_test {

// One request the server received, as it arrived.
struct LoopbackRequest {
    std::string method;
    std::string target; // path and query, as sent
    // Header names as sent, lowercased, with their values. A test that asserts on a header (the
    // cross-origin `Authorization` rule, #402) reads this rather than the raw bytes.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // The first value of `name` (case-insensitive), or an empty view.
    [[nodiscard]] std::string header(std::string_view name) const;
};

// What to answer with. The defaults are a 200 with an empty body, so a test only names what it
// cares about.
struct LoopbackResponse {
    int status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers{{"Content-Type", "text/plain"}};
    std::string body;

    // Whether to close the connection after this response. The default keeps it open, which is what
    // a back end with its own pooling expects; a test that wants "the server went away" sets it
    // true.
    bool close = false;
};

// A server that answers from a script, in order, and records what it was sent.
//
// The script is consumed one entry per request. A request past the end of the script gets the last
// entry again, so a test that expects N identical responses writes one and a test that expects a
// redirect chain writes each hop.
class LoopbackServer {
public:
    LoopbackServer() = default;
    ~LoopbackServer();

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    // Starts listening on 127.0.0.1 and an ephemeral port. Returns false when the socket could not
    // be bound, which on a machine with networking is a bug in this file rather than a test
    // failure.
    [[nodiscard]] bool start(std::vector<LoopbackResponse> script);

    // Stops the accept loop and closes the listener. Idempotent; the destructor calls it.
    void stop();

    // Whether the server is listening. A test that needs it (most do) asserts this, so a failed
    // bind is a named failure rather than a mysterious connect error from the back end.
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // The URL prefix to hand a back end: `http://127.0.0.1:<port>`.
    [[nodiscard]] std::string base_url() const;

    // The port, for a test that wants to build its own URL.
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    // The requests received so far, in arrival order.
    [[nodiscard]] std::vector<LoopbackRequest> requests() const;

    // How many requests arrived. A test that asserts "the redirect was not followed" reads this: a
    // back end that followed it would have made two.
    [[nodiscard]] std::size_t request_count() const;

    // Waits until at least `count` requests have arrived, or `timeout_ms` elapses. Returns whether
    // the count was reached.
    //
    // **Why a wait and not a sleep.** A back end completes on the completion queue and the test
    // drains that by hand, but the *server* side is a real socket read on another thread - there is
    // no manual clock that can advance it. Polling with a bound is the honest shape: the test says
    // how many requests it expects and how long it is willing to wait, and a timeout is a failure
    // the test can name.
    [[nodiscard]] bool wait_for_requests(std::size_t count, int timeout_ms = 2000);

private:
    void accept_loop();

    // One connection: read the request, answer from the script, close or continue.
    void serve_connection(int fd);

    std::atomic<bool> running_{false};
    std::uint16_t port_ = 0;
    int listener_ = -1;
    std::thread thread_;

    mutable std::mutex mutex_;
    std::condition_variable arrival_;
    std::vector<LoopbackResponse> script_;
    std::vector<LoopbackRequest> requests_;
    std::size_t served_ = 0;
};

} // namespace tessera::net_test
