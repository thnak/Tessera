// SPDX-License-Identifier: MIT

#include "tessera/harness/stdio_transport.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

using tessera::harness::StdioIoResult;
using tessera::harness::StdioIoStatus;
using tessera::harness::StdioStream;
using tessera::harness::StdioTransport;
using tessera::harness::TransportErrorCode;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_stdio_transport: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Deterministic stream: every read/write chunk size, stall, EOF, and failure is scripted, so a
// test never depends on a real handle or on timing.
class FakeStdioStream final : public StdioStream {
public:
    [[nodiscard]] StdioIoResult read_some(std::span<char> buffer) override {
        ++read_calls;
        if (read_error) {
            return {StdioIoStatus::Error, 0};
        }
        if (input_closed) {
            return {StdioIoStatus::EndOfStream, 0};
        }
        if (read_position == input.size()) {
            return input_complete ? StdioIoResult{StdioIoStatus::EndOfStream, 0}
                                  : StdioIoResult{StdioIoStatus::WouldBlock, 0};
        }
        const std::size_t count =
            std::min({buffer.size(), read_chunk, input.size() - read_position});
        std::memcpy(buffer.data(), input.data() + read_position, count);
        read_position += count;
        return {StdioIoStatus::Ok, count};
    }

    [[nodiscard]] StdioIoResult write_some(std::string_view bytes) override {
        ++write_calls;
        if (write_broken) {
            return {StdioIoStatus::EndOfStream, 0};
        }
        if (write_blocked) {
            return {StdioIoStatus::WouldBlock, 0};
        }
        if (write_error) {
            return {StdioIoStatus::Error, 0};
        }
        const std::size_t count = std::min(bytes.size(), write_chunk);
        written.append(bytes.substr(0, count));
        return {StdioIoStatus::Ok, count};
    }

    void append_input(std::string_view bytes) {
        input.append(bytes);
    }

    std::string input;
    std::size_t read_position = 0;
    std::size_t read_chunk = 4096;
    bool input_complete = true;
    bool input_closed = false;
    bool read_error = false;
    std::size_t read_calls = 0;

    std::string written;
    std::size_t write_chunk = 4096;
    bool write_blocked = false;
    bool write_broken = false;
    bool write_error = false;
    std::size_t write_calls = 0;
};

struct Fixture {
    explicit Fixture(std::size_t max_line_bytes = 4096, std::size_t queue_bytes = 16384)
        : stream_owner(std::make_unique<FakeStdioStream>()),
          stream(*stream_owner),
          transport(std::move(stream_owner), max_line_bytes, queue_bytes) {}

    FakeStdioStream* operator->() noexcept {
        return &stream;
    }

    std::unique_ptr<FakeStdioStream> stream_owner;
    FakeStdioStream& stream;
    StdioTransport transport;
};

std::string read_or(Fixture& fixture, const char* message) {
    const auto result = fixture.transport.read_line();
    if (!result.has_value()) {
        expect(false, message);
        return {};
    }
    return *result;
}

} // namespace

int main() {
    // A line split across many underlying reads is assembled by the framer and never half
    // delivered.
    {
        Fixture fixture;
        fixture->input_complete = false;
        fixture->append_input("{\"id\":1}\n");
        fixture->read_chunk = 2;
        expect(read_or(fixture, "split read") == "{\"id\":1}", "payload survives split reads");
        expect(fixture->read_calls > 2, "the fake really delivered small chunks");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::WouldBlock,
               "a drained but open stream would block");
    }

    // WouldBlock is not EOF: the peer can still append bytes, and the partial line is preserved.
    {
        Fixture fixture;
        fixture->input_complete = false;
        fixture->append_input("{\"id\":");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::WouldBlock,
               "partial line would block");
        fixture->append_input("2}\n");
        expect(read_or(fixture, "line completed later") == "{\"id\":2}",
               "partial bytes were retained");
    }

    // A final unterminated line at EOF is a frame; after it the transport reports Closed, once.
    {
        Fixture fixture;
        fixture->append_input("{\"id\":9}");
        expect(read_or(fixture, "eof tail") == "{\"id\":9}", "unterminated tail is a frame");
        expect(fixture.transport.input_at_eof(), "eof recorded");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::Closed,
               "closed after the tail");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::Closed,
               "closed is sticky");
        expect(fixture.transport.closed(), "closed() agrees");
    }

    // Framing rejections are typed, dropped whole, and keep their stream position.
    {
        Fixture fixture{8, 32};
        fixture->append_input("0123456789\nok\n\xC0\x80\n\nlast\n");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::LineTooLong,
               "oversized line rejected before the next line");
        expect(read_or(fixture, "line after oversize") == "ok", "resume after oversize");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::InvalidUtf8,
               "invalid utf-8 rejected");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::EmptyLine,
               "empty line rejected");
        expect(read_or(fixture, "line after empty") == "last", "resume after empty line");
    }

    // The cap is exact on both sides.
    {
        Fixture fixture{5, 32};
        fixture->append_input("12345\n123456\n");
        expect(read_or(fixture, "exact cap read") == "12345", "exact cap accepted");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::LineTooLong,
               "cap + 1 rejected");
        expect(fixture.transport.max_line_bytes() == 5, "cap advertised");
    }

    // Writes are framed with one LF and validated before anything reaches the stream.
    {
        Fixture fixture;
        expect(fixture.transport.write_line("{\"a\":1}").has_value(), "write accepted");
        expect(fixture->written == "{\"a\":1}\n", "writer owns the terminator");
        expect(fixture.transport.write_line("").error().code == TransportErrorCode::EmptyLine,
               "empty write rejected");
        expect(fixture.transport.write_line("a\nb").error().code ==
                   TransportErrorCode::EmbeddedTerminator,
               "embedded LF rejected");
        expect(fixture.transport.write_line("a\rb").error().code ==
                   TransportErrorCode::EmbeddedTerminator,
               "embedded CR rejected");
        expect(fixture.transport.write_line("\xC0\x80").error().code ==
                   TransportErrorCode::InvalidUtf8,
               "invalid utf-8 write rejected");
        expect(fixture->written == "{\"a\":1}\n", "rejected writes wrote nothing");
        expect(fixture.transport.write_line(std::string(4097, 'x')).error().code ==
                   TransportErrorCode::LineTooLong,
               "over-cap write rejected");
    }

    // A partial OS write is retried to completion without duplicating or reordering bytes.
    {
        Fixture fixture;
        fixture->write_chunk = 2;
        expect(fixture.transport.write_line("abc").has_value(), "short-chunk write accepted");
        expect(fixture->written == "abc\n", "short chunks reassembled in order");
        expect(fixture.transport.pending_write_bytes() == 0, "queue drained");
    }

    // A stalled stream keeps accepted bytes queued; flush_pending resumes them in order.
    {
        Fixture fixture;
        expect(fixture.transport.write_line("first").has_value(), "first write");
        fixture->write_blocked = true;
        expect(fixture.transport.write_line("second").has_value(), "queued write is accepted");
        expect(fixture.transport.pending_write_bytes() == 7, "seven framed bytes queued");
        expect(fixture.transport.flush_pending().error().code == TransportErrorCode::WouldBlock,
               "flush reports the stall");
        fixture->write_blocked = false;
        expect(fixture.transport.flush_pending().has_value(), "flush resumes");
        expect(fixture->written == "first\nsecond\n", "queued order preserved");
        expect(fixture.transport.pending_write_bytes() == 0, "queue empty");
    }

    // The queue is bounded: a frame that no longer fits is refused without writing a prefix, so a
    // caller can retry the same frame.
    {
        Fixture fixture{8, 10}; // cap clamps to max_line + 2 = 10
        expect(fixture.transport.write_queue_bytes() == 10, "queue cap clamped to one frame");
        fixture->write_blocked = true;
        expect(fixture.transport.write_line("aaaaaaaa").has_value(), "first frame queued");
        expect(fixture.transport.pending_write_bytes() == 9, "nine bytes queued");
        expect(fixture.transport.write_line("bbbbbbbb").error().code ==
                   TransportErrorCode::QueueFull,
               "full queue refused");
        expect(fixture.transport.pending_write_bytes() == 9, "refusal wrote nothing");
        fixture->write_blocked = false;
        expect(fixture.transport.flush_pending().has_value(), "queue flushed");
        expect(fixture->written == "aaaaaaaa\n", "only the accepted frame was written");
        expect(fixture.transport.write_line("bbbbbbbb").has_value(), "retry after flush");
        expect(fixture->written == "aaaaaaaa\nbbbbbbbb\n", "retry wrote exactly once");
    }

    // EOF on input does not close output: the final responses still go out, then closed() is true.
    {
        Fixture fixture;
        expect(fixture.transport.read_line().error().code == TransportErrorCode::Closed,
               "empty input is EOF");
        expect(fixture.transport.write_line("bye").has_value(), "output survives input EOF");
        expect(fixture->written == "bye\n", "final response written");
        expect(fixture.transport.closed(), "closed once output drained");
    }

    // A broken output stream closes the transport and drops the queue.
    {
        Fixture fixture;
        fixture->write_broken = true;
        expect(fixture.transport.write_line("x").error().code == TransportErrorCode::Closed,
               "broken pipe reports Closed");
        expect(fixture.transport.closed(), "broken pipe closes");
        expect(fixture.transport.flush_pending().error().code == TransportErrorCode::Closed,
               "flush after close");
        expect(fixture.transport.read_line().error().code == TransportErrorCode::Closed,
               "read after close");
    }

    // Input errors close the transport instead of being mistaken for EOF/WouldBlock.
    {
        Fixture fixture;
        fixture->read_error = true;
        expect(fixture.transport.read_line().error().code == TransportErrorCode::IoError,
               "read error reported");
        expect(fixture.transport.closed(), "read error closes");
    }

    // close() is idempotent and drops output the stream never accepted.
    {
        Fixture fixture;
        fixture->write_blocked = true;
        expect(fixture.transport.write_line("held").has_value(), "frame queued");
        fixture.transport.close();
        expect(fixture.transport.closed(), "close marks closed");
        expect(fixture.transport.pending_write_bytes() == 0, "close drops the queue");
        expect(fixture.transport.write_line("after").error().code == TransportErrorCode::Closed,
               "write after close");
        fixture.transport.close();
        expect(fixture.transport.closed(), "close is idempotent");
        expect(fixture->written.empty(), "dropped bytes never reached the stream");
    }

    if (failures != 0) {
        std::cerr << "harness_stdio_transport: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
