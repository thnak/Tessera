// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/line_framing.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::harness {

// The only transport in the v1 protocol boundary is a byte stream that already belongs to the
// harness process (stdin/stdout, or an in-process pipe). There is deliberately no socket, no
// token, and no auth code here: the protocol is local-only
// (docs/specs/headless-automation-v1.md sections 6 and 9), and authentication of a peer is a
// property of how the process was started, not of the codec.
enum class TransportErrorCode {
    Closed,             // the stream or the peer is gone
    WouldBlock,         // no complete line yet; the peer is still open, so try again later
    LineTooLong,        // payload exceeds the transport's documented line cap
    InvalidUtf8,        // payload is not well-formed UTF-8
    EmptyLine,          // an empty line is not a frame
    EmbeddedTerminator, // payload carries a CR or LF, which would smuggle a second frame
    QueueFull,          // accepted output is capped; retry after flush() drains some of it
    IoError,            // the underlying stream failed
};

struct TransportError {
    TransportErrorCode code = TransportErrorCode::IoError;
    std::string detail;

    friend bool operator==(const TransportError&, const TransportError&) = default;
};

// Line-oriented transport seam. Implementations never block: a read either returns one complete
// line, `WouldBlock` while the peer is still open, or `Closed`.
class LineTransport {
public:
    LineTransport() = default;
    LineTransport(const LineTransport&) = delete;
    LineTransport& operator=(const LineTransport&) = delete;
    virtual ~LineTransport() = default;

    // One frame payload without its terminator, validated as UTF-8 and bounded by
    // max_line_bytes(). Empty lines are rejected, never returned as empty strings.
    [[nodiscard]] virtual std::expected<std::string, TransportError> read_line() = 0;

    // Writes one frame payload; the implementation owns the terminator. A payload that already
    // contains CR/LF, exceeds the cap, is empty, or is not UTF-8 is rejected instead of written.
    // Transports with an internal buffer may accept a frame before the peer reads it; flush()
    // pushes buffered bytes without waiting, and a full buffer reports QueueFull.
    [[nodiscard]] virtual std::expected<void, TransportError> write_line(std::string_view line) = 0;

    // Pushes buffered output without waiting. The default is a no-op because a transport that
    // delivers synchronously (the in-process pair) has nothing to flush.
    [[nodiscard]] virtual std::expected<void, TransportError> flush() {
        return {};
    }

    // Bounded readiness wait for a serve loop that has nothing else to do. Returns true when
    // read_line() can make progress (a line, a framing error, EOF, or a closed transport) and
    // false when the timeout elapsed first. It never consumes input and never waits longer than
    // `timeout`, so the caller stays interruptible. The default reports "ready" at once, which is
    // correct for transports that deliver synchronously and never return WouldBlock.
    [[nodiscard]] virtual bool wait_readable(std::chrono::milliseconds timeout) {
        (void)timeout;
        return true;
    }

    [[nodiscard]] virtual std::size_t max_line_bytes() const noexcept = 0;
    [[nodiscard]] virtual bool closed() const noexcept = 0;
    // Idempotent. After close(), reads report Closed and writes fail; bytes already queued by the
    // peer remain readable until they are drained.
    virtual void close() noexcept = 0;
};

} // namespace tessera::harness
