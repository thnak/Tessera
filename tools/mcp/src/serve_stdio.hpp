// SPDX-License-Identifier: MIT

#pragma once

#include "session.hpp"
#include "tessera/harness/line_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>

// The standalone stdio loop of `tessera-mcp`: read one line, dispatch it, write at most one line,
// until the peer closes stdin or `stop` is set. It is the same shape as the harness serve loop
// (src/harness/serve.cpp) and reuses the harness transport seam rather than reading stdin directly,
// so the framing rules -- bounded line length, UTF-8 validation, CRLF tolerance, EOF-terminated
// last line, non-blocking readiness -- are the harness's, not a second copy.
namespace tessera::mcp {

struct ServeOptions {
    // Longest single readiness wait, which is also how often `stop` is observed while the client is
    // silent. It bounds shutdown latency and must stay small (the harness uses 50 ms).
    std::chrono::milliseconds wait_slice{50};
    // A client that is not reading must not make the server spin: one short sleep per blocked write
    // is the same policy the harness loop uses.
    std::chrono::milliseconds output_blocked_sleep{1};
    // Set by another thread (a test, a signal flag) to end the loop. May be null.
    const std::atomic<bool>* stop = nullptr;
};

struct ServeResult {
    std::size_t lines_read = 0;
    std::size_t lines_written = 0;
    std::size_t rejected_frames = 0; // framing errors, each answered with a null-id parse error
    bool stopped = false;            // the stop flag was observed
    // Set when the session could not encode an answer, which is unrecoverable: the server can no
    // longer keep its side of the conversation, so the loop ends rather than dropping a request.
    bool failed = false;
};

// Serves `session` until the input closes. Never creates a thread and never waits beyond the
// bounded readiness wait above, so a client that stops sending still has its `stop` flag observed.
// The session is the caller's so a test can inspect the state a run produced (the project it has
// open, the fact that it accepted the handshake) after the loop has ended.
[[nodiscard]] ServeResult serve_stdio(harness::LineTransport& transport, McpSession& session,
                                      ServeOptions options = {});

} // namespace tessera::mcp
