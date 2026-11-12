// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/line_transport.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace tessera::harness {

// Outcome of one raw stream operation. `WouldBlock` means the handle is open and healthy but has
// no bytes (read) or no room (write) right now, so the caller must not wait for it.
enum class StdioIoStatus {
    Ok,
    WouldBlock,
    EndOfStream,
    Error,
};

struct StdioIoResult {
    StdioIoStatus status = StdioIoStatus::Ok;
    std::size_t bytes = 0;
};

// Byte-level seam under StdioTransport. The production implementation is the process's stdin and
// stdout; tests inject an in-memory stream so no test touches a real handle and every partial
// read/write is reproducible.
//
// read_some never waits: the OS implementation probes readiness with a zero-timeout poll (POSIX)
// or a zero-timeout wait/peek (Windows) before touching the handle. It also never changes the
// handle's blocking mode; a caller that opened stdin non-blocking keeps ownership of that flag.
// One limitation is inherent to an interactive console in line-input mode: the OS itself may wait
// for a complete line once it has been asked for input, which is why the harness CLI is normally
// driven by a pipe. A pipe or a file never waits here.
class StdioStream {
public:
    StdioStream() = default;
    StdioStream(const StdioStream&) = delete;
    StdioStream& operator=(const StdioStream&) = delete;
    virtual ~StdioStream() = default;

    [[nodiscard]] virtual StdioIoResult read_some(std::span<char> buffer) = 0;
    [[nodiscard]] virtual StdioIoResult write_some(std::string_view bytes) = 0;

    // Waits up to `timeout` for read_some to be able to make progress (bytes, EOF, or an error
    // that read_some will report). Never reads. The default says "ready", so a stream without a
    // wait primitive degrades to the caller's own bounded retry rather than blocking.
    // POSIX uses poll(2) with the timeout. Windows cannot reliably wait on a pipe or file
    // handle, so it sleeps in short bounded slices between PeekNamedPipe probes (no reader
    // thread, hence nothing to join); a console handle uses WaitForSingleObject and a disk file
    // is always ready.
    [[nodiscard]] virtual bool wait_readable(std::chrono::milliseconds timeout) {
        (void)timeout;
        return true;
    }
};

// stdin/stdout in binary mode. Handles are owned by the process, not by the returned stream.
[[nodiscard]] std::unique_ptr<StdioStream> make_process_stdio_stream();

// Unwritten bytes the transport will accept before write_line starts reporting QueueFull. A
// single frame is at most kMaxLineBytes, so the default holds one maximum frame plus a burst.
inline constexpr std::size_t kStdioWriteQueueBytes = std::size_t{2} << 20U;

// LineTransport over a capped, non-blocking stdio stream.
//
// Read side: bytes are buffered incrementally by LineFramer, so a payload split over many reads
// is never observable. `WouldBlock` is propagated unchanged; EOF flushes the pending unterminated
// line (an EOF-terminated frame) and then reports Closed. A framing rejection (over-long line,
// bad UTF-8, empty line) is reported once and framing resumes after the terminator.
//
// Write side: write_line validates and frames the payload, appends it to a bounded queue, and
// then flushes as much of the queue as the stream accepts without waiting. Success therefore
// means "accepted for delivery", not "the peer has read it". A partial OS write keeps the
// remainder queued; call flush_pending() (or the next write_line) to continue. The queue cap is
// clamped to at least max_line_bytes + 2 so one valid frame always fits. When the queue is full,
// write_line fails with QueueFull and writes nothing, so a caller can retry the same line
// without duplicating bytes.
class StdioTransport final : public LineTransport {
public:
    static constexpr std::size_t kDefaultWriteQueueBytes = kStdioWriteQueueBytes;

    explicit StdioTransport(std::unique_ptr<StdioStream> stream,
                            std::size_t max_line_bytes = kMaxLineBytes,
                            std::size_t write_queue_bytes = kDefaultWriteQueueBytes);
    ~StdioTransport() override;

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    [[nodiscard]] std::expected<std::string, TransportError> read_line() override;
    [[nodiscard]] std::expected<void, TransportError> write_line(std::string_view line) override;
    [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) override;
    [[nodiscard]] std::size_t max_line_bytes() const noexcept override;
    [[nodiscard]] bool closed() const noexcept override;
    void close() noexcept override;

    // Retries queued output. Succeeds when the queue is empty, WouldBlock/QueueFull when the
    // stream still cannot accept everything, and Closed/IoError when the stream failed.
    [[nodiscard]] std::expected<void, TransportError> flush_pending();

    [[nodiscard]] std::size_t pending_write_bytes() const noexcept {
        return pending_.size();
    }
    [[nodiscard]] bool input_at_eof() const noexcept {
        return input_eof_;
    }
    [[nodiscard]] std::size_t write_queue_bytes() const noexcept {
        return write_queue_bytes_;
    }

private:
    // Flushes the queue once. Returns the first failure, or success when the queue drained.
    [[nodiscard]] std::expected<void, TransportError> flush_once();
    [[nodiscard]] std::unexpected<TransportError> fail(TransportErrorCode code,
                                                       std::string detail) noexcept;

    std::unique_ptr<StdioStream> stream_;
    LineFramer framer_;
    std::string pending_; // framed but not yet accepted by the stream
    std::size_t max_line_bytes_;
    std::size_t write_queue_bytes_;
    bool closed_ = false;
    bool input_eof_ = false;
};

} // namespace tessera::harness
