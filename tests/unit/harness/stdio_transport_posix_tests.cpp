// SPDX-License-Identifier: MIT

// Real POSIX process-stdio coverage for `ProcessStdioStream` (src/harness/stdio_transport.cpp, the
// `#else`/poll(2) branch). The production stream resolves STDIN_FILENO and STDOUT_FILENO at every
// call instead of caching handles, so pointing those two descriptors at pipes for the duration of
// one case is enough to drive the real poll(2)/read(2)/write(2) path. Every other stdio suite
// injects `FakeStdioStream`; none of them can catch a descriptor-level mistake (wrong fd, lost EOF,
// EPIPE, a wait that does not honor its timeout).
//
// Built only on UNIX (tests/unit/harness/CMakeLists.txt). No threads: each case writes every byte
// it will observe before the wait or read that observes it, and every wait is timeout-bounded, so
// the test is a fixed syscall sequence rather than a race against a peer thread.

#include "tessera/harness/stdio_transport.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using tessera::harness::make_process_stdio_stream;
using tessera::harness::StdioIoStatus;
using tessera::harness::StdioTransport;
using tessera::harness::TransportErrorCode;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_stdio_transport_posix: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Closes an owned descriptor exactly once; -1 marks "already closed", so the fixture can never
// double-close or close a descriptor that some other part of the process has since reopened.
void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t count = ::write(fd, bytes.data(), bytes.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

// Reads exactly `count` bytes. The pipe is blocking, but the caller only asks for bytes it has
// already written, so this never waits on a peer.
std::string read_exactly(int fd, std::size_t count) {
    std::string out(count, '\0');
    std::size_t got = 0;
    while (got < count) {
        const ssize_t got_bytes = ::read(fd, out.data() + got, count - got);
        if (got_bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (got_bytes == 0) {
            break;
        }
        got += static_cast<std::size_t>(got_bytes);
    }
    out.resize(got);
    return out;
}

// Replaces process stdin/stdout with pipe ends for one scope and restores the saved descriptors on
// every exit path (a failed assertion only increments a counter, it does not unwind). Descriptors
// are process-global, so exactly one fixture exists at a time; each case below owns its own.
class StdioPipeFixture {
public:
    StdioPipeFixture() {
        saved_stdin_ = ::dup(STDIN_FILENO);
        saved_stdout_ = ::dup(STDOUT_FILENO);
        if (saved_stdin_ < 0 || saved_stdout_ < 0) {
            expect(false, "could not duplicate the process stdio descriptors");
            return;
        }
        if (!make_pipe(input_pipe_) || !make_pipe(output_pipe_)) {
            expect(false, "could not create the stdio pipes");
            return;
        }
        if (::dup2(input_pipe_[0], STDIN_FILENO) < 0 ||
            ::dup2(output_pipe_[1], STDOUT_FILENO) < 0) {
            expect(false, "could not redirect stdin/stdout onto the pipes");
            return;
        }
        // stdin/stdout now own duplicates of these ends. Dropping the originals keeps ownership
        // unambiguous: one descriptor per end, so closing input_pipe_[1] is a genuine EOF for
        // stdin instead of a leaked copy that silently keeps the pipe writable.
        close_fd(input_pipe_[0]);
        close_fd(output_pipe_[1]);
        ready_ = true;
    }

    ~StdioPipeFixture() {
        restore();
    }

    StdioPipeFixture(const StdioPipeFixture&) = delete;
    StdioPipeFixture& operator=(const StdioPipeFixture&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return ready_;
    }

    // Writes bytes the stream will read. A short write is retried; the pipe always has room because
    // each case writes at most a few dozen bytes.
    [[nodiscard]] bool write_input(std::string_view bytes) const {
        return write_all(input_pipe_[1], bytes);
    }

    // Closing the write end is how the test produces EOF on stdin; STDIN_FILENO keeps the read end
    // open so read(2) can return 0 instead of failing.
    [[nodiscard]] bool close_input_writer() {
        if (input_pipe_[1] < 0) {
            return false;
        }
        close_fd(input_pipe_[1]);
        return true;
    }

    // Closing the read end makes the next stdout write fail with EPIPE (and SIGPIPE unless the
    // caller ignores it).
    [[nodiscard]] bool close_output_reader() {
        if (output_pipe_[0] < 0) {
            return false;
        }
        close_fd(output_pipe_[0]);
        return true;
    }

    [[nodiscard]] std::string read_output(std::size_t bytes) const {
        return read_exactly(output_pipe_[0], bytes);
    }

private:
    static bool make_pipe(int (&ends)[2]) {
        int created[2] = {-1, -1};
        if (::pipe(created) != 0) {
            return false;
        }
        ends[0] = created[0];
        ends[1] = created[1];
        return true;
    }

    void restore() noexcept {
        // Restore first: the process's real stdio must be back before the pipe ends below are
        // closed, so nothing that runs later can write into a pipe whose reader is gone.
        if (saved_stdin_ >= 0) {
            ::dup2(saved_stdin_, STDIN_FILENO);
            close_fd(saved_stdin_);
        }
        if (saved_stdout_ >= 0) {
            ::dup2(saved_stdout_, STDOUT_FILENO);
            close_fd(saved_stdout_);
        }
        close_fd(input_pipe_[0]);
        close_fd(input_pipe_[1]);
        close_fd(output_pipe_[0]);
        close_fd(output_pipe_[1]);
    }

    int saved_stdin_ = -1;
    int saved_stdout_ = -1;
    int input_pipe_[2] = {-1, -1};  // [0] read end (becomes stdin), [1] write end (EOF control)
    int output_pipe_[2] = {-1, -1}; // [0] read end (test reads), [1] write end (becomes stdout)
    bool ready_ = false;
};

std::string read_or(StdioTransport& transport, const char* message) {
    const auto line = transport.read_line();
    if (!line.has_value()) {
        expect(false, message);
        return {};
    }
    return *line;
}

} // namespace

int main() {
    using std::chrono::milliseconds;

    // (a) An empty pipe is not readable. The wait has to spend the requested time inside poll(2);
    // the bounds are deliberately loose (a third of the timeout down, two seconds up) because this
    // asserts that the OS timer is honored, not how quickly a loaded machine schedules the process.
    {
        StdioPipeFixture fixture;
        if (expect(fixture.valid(), "stdin/stdout could not be redirected onto pipes")) {
            const auto stream = make_process_stdio_stream();
            const auto start = std::chrono::steady_clock::now();
            const bool ready = stream->wait_readable(milliseconds(150));
            const auto elapsed =
                std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - start);
            expect(!ready, "an empty pipe is not readable");
            expect(elapsed >= milliseconds(50), "the wait honored a meaningful timeout");
            expect(elapsed <= milliseconds(2000), "the wait did not overshoot the timeout wildly");

            // This is the zero-timeout probe every read starts with: it must report WouldBlock
            // instead of waiting for bytes that are not there.
            std::array<char, 8> bytes{};
            expect(stream->read_some(bytes).status == StdioIoStatus::WouldBlock,
                   "read_some does not block on an empty pipe");
        }
    }

    // (b) Bytes already in the pipe make the wait return at once. The timeout is much longer than
    // the accepted elapsed time, so a wait that merely timed out cannot pass this case.
    {
        StdioPipeFixture fixture;
        if (expect(fixture.valid(), "stdin/stdout could not be redirected onto pipes")) {
            const auto stream = make_process_stdio_stream();
            const std::string payload = "ready-\xE2\x9C\x93";
            expect(fixture.write_input(payload), "input bytes written");
            const auto start = std::chrono::steady_clock::now();
            expect(stream->wait_readable(milliseconds(5000)), "written bytes are readable");
            const auto elapsed =
                std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - start);
            expect(elapsed < milliseconds(1000), "readiness came before the timeout");

            std::array<char, 32> bytes{};
            const auto result = stream->read_some(bytes);
            expect(result.status == StdioIoStatus::Ok && result.bytes == payload.size(),
                   "read_some reported every byte");
            expect(std::string_view(bytes.data(), result.bytes) == payload,
                   "read_some returned the bytes byte-identically");
            expect(stream->read_some(bytes).status == StdioIoStatus::WouldBlock,
                   "the pipe is drained after the read");
        }
    }

    // (c) EOF is readiness: poll(2) reports the hangup, read(2) returns 0, and read_some reports
    // EndOfStream. A final unterminated line is still a frame for the transport, which then closes.
    {
        StdioPipeFixture fixture;
        if (expect(fixture.valid(), "stdin/stdout could not be redirected onto pipes")) {
            const auto stream = make_process_stdio_stream();
            expect(fixture.close_input_writer(), "input writer closed");
            const auto start = std::chrono::steady_clock::now();
            expect(stream->wait_readable(milliseconds(5000)), "EOF is readable");
            const auto elapsed =
                std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - start);
            expect(elapsed < milliseconds(1000), "EOF readiness did not wait for the timeout");
            std::array<char, 8> bytes{};
            expect(stream->read_some(bytes).status == StdioIoStatus::EndOfStream,
                   "read_some reports EndOfStream");
        }
    }
    {
        StdioPipeFixture fixture;
        if (expect(fixture.valid(), "stdin/stdout could not be redirected onto pipes")) {
            expect(fixture.write_input("tail"), "tail bytes written");
            expect(fixture.close_input_writer(), "input writer closed");
            StdioTransport transport(make_process_stdio_stream());
            expect(transport.wait_readable(milliseconds(5000)), "the transport sees EOF readiness");
            expect(read_or(transport, "EOF-terminated tail") == "tail",
                   "an unterminated tail at EOF is a frame");
            const auto after = transport.read_line();
            expect(!after.has_value() && after.error().code == TransportErrorCode::Closed,
                   "read_line reports Closed after the tail");
            expect(transport.closed(), "closed() agrees");
        }
    }

    // (d) Output goes through the real stdout descriptor: write_some is compared byte for byte,
    // write_line adds exactly one LF, and a closed peer surfaces as EndOfStream.
    {
        StdioPipeFixture fixture;
        if (expect(fixture.valid(), "stdin/stdout could not be redirected onto pipes")) {
            const auto stream = make_process_stdio_stream();
            const std::string payload = "stdout-\xE2\x9C\x93";
            const auto written = stream->write_some(payload);
            expect(written.status == StdioIoStatus::Ok && written.bytes == payload.size(),
                   "write_some accepted every byte");
            expect(fixture.read_output(payload.size()) == payload,
                   "write_some round-trips byte-identically");

            StdioTransport transport(make_process_stdio_stream());
            expect(transport.write_line("{\"id\":7}").has_value(), "write_line accepted the frame");
            expect(fixture.read_output(std::string_view("{\"id\":7}\n").size()) == "{\"id\":7}\n",
                   "write_line frames with exactly one LF");
            expect(transport.pending_write_bytes() == 0, "the real write drained the queue");

            // EPIPE path. SIGPIPE is ignored only for this one write and the previous disposition
            // is restored immediately, so the rest of the binary never runs under a changed signal
            // setup; without the ignore the process would die instead of seeing EPIPE.
            expect(fixture.close_output_reader(), "output reader closed");
            const auto previous = std::signal(SIGPIPE, SIG_IGN);
            const auto broken = stream->write_some("x");
            if (previous != SIG_ERR) {
                std::signal(SIGPIPE, previous);
            }
            expect(broken.status == StdioIoStatus::EndOfStream,
                   "a closed stdout peer is EndOfStream, not SIGPIPE");
        }
    }

    // (e) StdioTransport::wait_readable delegates to the real stream only when it has no buffered
    // complete line, and its read side frames lines arriving through the real descriptor.
    {
        StdioPipeFixture fixture;
        if (expect(fixture.valid(), "stdin/stdout could not be redirected onto pipes")) {
            StdioTransport transport(make_process_stdio_stream());
            expect(fixture.write_input("first\nsecond\npart"), "input frames written");
            expect(transport.wait_readable(milliseconds(5000)),
                   "stream readiness is reported through the transport");
            expect(read_or(transport, "first line") == "first", "first line framed");
            expect(transport.wait_readable(milliseconds(0)),
                   "a buffered complete line is ready without asking the stream");
            expect(read_or(transport, "second line") == "second", "second line framed");
            const auto partial = transport.read_line();
            expect(!partial.has_value() && partial.error().code == TransportErrorCode::WouldBlock,
                   "a partial line is WouldBlock, not a frame");

            // Same loose bounds as (a): the transport must pass the wait to the descriptor, and a
            // half-delivered frame must not look ready.
            const auto start = std::chrono::steady_clock::now();
            expect(!transport.wait_readable(milliseconds(100)), "a partial line is not ready");
            const auto elapsed =
                std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - start);
            expect(elapsed >= milliseconds(40), "the delegated wait honored its timeout");
            expect(elapsed <= milliseconds(2000), "the delegated wait did not overshoot wildly");

            expect(fixture.write_input("ial\n"), "tail bytes written");
            expect(transport.wait_readable(milliseconds(5000)), "completed line is ready");
            expect(read_or(transport, "completed line") == "partial",
                   "the split line is reassembled");
        }
    }

    if (failures != 0) {
        std::cerr << "harness_stdio_transport_posix: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
