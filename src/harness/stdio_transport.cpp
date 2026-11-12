// SPDX-License-Identifier: MIT

#include "tessera/harness/stdio_transport.hpp"
#include "tessera/harness/utf8.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace tessera::harness {
namespace {

TransportErrorCode to_transport_code(FrameErrorCode code) noexcept {
    switch (code) {
    case FrameErrorCode::LineTooLong:
        return TransportErrorCode::LineTooLong;
    case FrameErrorCode::InvalidUtf8:
        return TransportErrorCode::InvalidUtf8;
    case FrameErrorCode::EmptyLine:
        return TransportErrorCode::EmptyLine;
    }
    return TransportErrorCode::IoError;
}

std::string describe(const FrameError& error) {
    switch (error.code) {
    case FrameErrorCode::LineTooLong:
        return "line exceeds the transport cap at offset " + std::to_string(error.offset);
    case FrameErrorCode::InvalidUtf8:
        return "line is not well-formed UTF-8 at offset " + std::to_string(error.offset);
    case FrameErrorCode::EmptyLine:
        return "empty line is not a frame";
    }
    return "framing error";
}

enum class ReadOutcome {
    More,
    WouldBlock,
    End,
    Error,
};

#if defined(_WIN32)

class ProcessStdioStream final : public StdioStream {
public:
    [[nodiscard]] StdioIoResult read_some(std::span<char> buffer) override {
        if (buffer.empty()) {
            return {StdioIoStatus::Ok, 0};
        }
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) {
            return {StdioIoStatus::Error, 0};
        }
        const DWORD type = GetFileType(input);
        if (type == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) == 0) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                    return {StdioIoStatus::EndOfStream, 0};
                }
                return {StdioIoStatus::Error, 0};
            }
            if (available == 0) {
                return {StdioIoStatus::WouldBlock, 0};
            }
        } else if (type != FILE_TYPE_DISK) {
            // A disk file is always ready. A console becomes ready when the user has typed
            // something; waiting on a synchronous file handle is not a documented operation, so
            // the disk case must skip the probe.
            const DWORD wait = WaitForSingleObject(input, 0);
            if (wait == WAIT_TIMEOUT) {
                return {StdioIoStatus::WouldBlock, 0};
            }
            if (wait == WAIT_FAILED) {
                return {StdioIoStatus::Error, 0};
            }
        }
        DWORD read = 0;
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(64) * 1024));
        if (ReadFile(input, buffer.data(), request, &read, nullptr) == 0) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                return {StdioIoStatus::EndOfStream, 0};
            }
            if (error == ERROR_NO_DATA) {
                return {StdioIoStatus::WouldBlock, 0};
            }
            return {StdioIoStatus::Error, 0};
        }
        if (read == 0) {
            return {StdioIoStatus::EndOfStream, 0};
        }
        return {StdioIoStatus::Ok, static_cast<std::size_t>(read)};
    }

    [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) override {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) {
            return true; // read_some reports the error
        }
        const DWORD type = GetFileType(input);
        if (type == FILE_TYPE_DISK) {
            return true;
        }
        if (type != FILE_TYPE_PIPE) {
            const DWORD wait = WaitForSingleObject(
                input, static_cast<DWORD>(std::max<std::chrono::milliseconds::rep>(
                           0, std::min<std::chrono::milliseconds::rep>(timeout.count(), 60'000))));
            return wait != WAIT_TIMEOUT;
        }
        // Pipes: bounded sleep slices between peeks. 2 ms caps the added latency and the CPU use
        // (about 500 wakeups/s only while the peer is silent and a wait is requested).
        constexpr auto kSlice = std::chrono::milliseconds(2);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            DWORD available = 0;
            if (PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) == 0 ||
                available > 0) {
                return true; // data, or a broken pipe that read_some reports as EOF/error
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            Sleep(static_cast<DWORD>(
                std::min<std::chrono::milliseconds>(
                    kSlice, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) +
                                std::chrono::milliseconds(1))
                    .count()));
        }
    }

    [[nodiscard]] StdioIoResult write_some(std::string_view bytes) override {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output == nullptr || output == INVALID_HANDLE_VALUE) {
            return {StdioIoStatus::Error, 0};
        }
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(64) * 1024));
        if (WriteFile(output, bytes.data(), request, &written, nullptr) == 0) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                return {StdioIoStatus::EndOfStream, 0};
            }
            if (error == ERROR_NO_DATA) {
                return {StdioIoStatus::WouldBlock, 0};
            }
            return {StdioIoStatus::Error, 0};
        }
        return {StdioIoStatus::Ok, static_cast<std::size_t>(written)};
    }
};

#else

class ProcessStdioStream final : public StdioStream {
public:
    [[nodiscard]] StdioIoResult read_some(std::span<char> buffer) override {
        if (buffer.empty()) {
            return {StdioIoStatus::Ok, 0};
        }
        for (;;) {
            pollfd descriptor{STDIN_FILENO, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 0);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return {StdioIoStatus::Error, 0};
            }
            if (ready == 0) {
                return {StdioIoStatus::WouldBlock, 0};
            }
            if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                return {StdioIoStatus::Error, 0};
            }
            break;
        }
        for (;;) {
            const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (count > 0) {
                return {StdioIoStatus::Ok, static_cast<std::size_t>(count)};
            }
            if (count == 0) {
                return {StdioIoStatus::EndOfStream, 0};
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {StdioIoStatus::WouldBlock, 0};
            }
            return {StdioIoStatus::Error, 0};
        }
    }

    [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining =
                now >= deadline
                    ? std::chrono::milliseconds(0)
                    : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) +
                          std::chrono::milliseconds(1);
            pollfd descriptor{STDIN_FILENO, POLLIN, 0};
            const int ready = ::poll(
                &descriptor, 1, static_cast<int>(std::min<long long>(remaining.count(), 60'000)));
            if (ready < 0 && errno == EINTR) {
                if (now >= deadline) {
                    return false;
                }
                continue; // retry with the recomputed remainder
            }
            return ready != 0; // data, hangup, or an error that read_some reports
        }
    }

    [[nodiscard]] StdioIoResult write_some(std::string_view bytes) override {
        for (;;) {
            const ssize_t count = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
            if (count >= 0) {
                return {StdioIoStatus::Ok, static_cast<std::size_t>(count)};
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {StdioIoStatus::WouldBlock, 0};
            }
            if (errno == EPIPE) {
                return {StdioIoStatus::EndOfStream, 0};
            }
            return {StdioIoStatus::Error, 0};
        }
    }
};

#endif

} // namespace

std::unique_ptr<StdioStream> make_process_stdio_stream() {
    return std::make_unique<ProcessStdioStream>();
}

StdioTransport::StdioTransport(std::unique_ptr<StdioStream> stream, std::size_t max_line_bytes,
                               std::size_t write_queue_bytes)
    : stream_(std::move(stream)),
      framer_(max_line_bytes),
      max_line_bytes_(max_line_bytes),
      // One valid frame plus its LF must always fit, otherwise a legal line could never be
      // accepted and the queue would be a permanent wall.
      write_queue_bytes_(std::max(write_queue_bytes, max_line_bytes + 2)) {
    if (stream_ == nullptr) {
        stream_ = make_process_stdio_stream();
    }
}

StdioTransport::~StdioTransport() {
    close();
}

std::size_t StdioTransport::max_line_bytes() const noexcept {
    return max_line_bytes_;
}

bool StdioTransport::closed() const noexcept {
    return closed_ || (input_eof_ && pending_.empty());
}

void StdioTransport::close() noexcept {
    closed_ = true;
    // Buffered bytes were never accepted by the stream; close() is the hard stop, so they are
    // dropped rather than delivered after the caller has declared the session over.
    pending_.clear();
}

std::unexpected<TransportError> StdioTransport::fail(TransportErrorCode code,
                                                     std::string detail) noexcept {
    return std::unexpected(TransportError{code, std::move(detail)});
}

std::expected<std::string, TransportError> StdioTransport::read_line() {
    if (closed_) {
        return fail(TransportErrorCode::Closed, "stdio transport is closed");
    }
    for (;;) {
        std::string line;
        FrameError frame_error;
        switch (framer_.next(line, frame_error)) {
        case LineFramer::Status::Line:
            return line;
        case LineFramer::Status::Rejected:
            return std::unexpected(
                TransportError{to_transport_code(frame_error.code), describe(frame_error)});
        case LineFramer::Status::EndOfStream:
            input_eof_ = true;
            return fail(TransportErrorCode::Closed, "stdin reached end of file");
        case LineFramer::Status::NeedMore: {
            if (input_eof_) {
                return fail(TransportErrorCode::Closed, "stdin reached end of file");
            }
            std::array<char, 8192> scratch{};
            const StdioIoResult result =
                stream_->read_some(std::span<char>(scratch.data(), scratch.size()));
            switch (result.status) {
            case StdioIoStatus::Ok:
                if (result.bytes == 0) {
                    return fail(TransportErrorCode::IoError, "stdin read returned no bytes");
                }
                framer_.append(std::string_view(scratch.data(), result.bytes));
                break;
            case StdioIoStatus::WouldBlock:
                return fail(TransportErrorCode::WouldBlock, "no complete line yet");
            case StdioIoStatus::EndOfStream:
                // An unterminated tail is still a frame; finish() guarantees the next call
                // cannot report NeedMore again.
                input_eof_ = true;
                framer_.finish();
                break;
            case StdioIoStatus::Error:
                closed_ = true;
                return fail(TransportErrorCode::IoError, "stdin read failed");
            }
            break;
        }
        }
    }
}

bool StdioTransport::wait_readable(std::chrono::milliseconds timeout) {
    if (closed_ || input_eof_ || framer_.ready()) {
        return true;
    }
    return stream_->wait_readable(timeout);
}

std::expected<void, TransportError> StdioTransport::flush_once() {
    while (!pending_.empty()) {
        if (closed_) {
            return fail(TransportErrorCode::Closed, "stdio transport is closed");
        }
        const StdioIoResult result = stream_->write_some(pending_);
        switch (result.status) {
        case StdioIoStatus::Ok: {
            const std::size_t accepted = std::min(result.bytes, pending_.size());
            if (accepted == 0) {
                return fail(TransportErrorCode::WouldBlock, "stdout accepted no bytes");
            }
            pending_.erase(0, accepted);
            break;
        }
        case StdioIoStatus::WouldBlock:
            return fail(TransportErrorCode::WouldBlock, "stdout would block");
        case StdioIoStatus::EndOfStream:
            closed_ = true;
            pending_.clear();
            return fail(TransportErrorCode::Closed, "stdout closed by the peer");
        case StdioIoStatus::Error:
            closed_ = true;
            pending_.clear();
            return fail(TransportErrorCode::IoError, "stdout write failed");
        }
    }
    return {};
}

std::expected<void, TransportError> StdioTransport::write_line(std::string_view line) {
    if (closed_) {
        return fail(TransportErrorCode::Closed, "stdio transport is closed");
    }
    if (line.empty()) {
        return fail(TransportErrorCode::EmptyLine, "an empty line is not a frame");
    }
    if (line.size() > max_line_bytes_) {
        return fail(TransportErrorCode::LineTooLong, "line exceeds the transport cap");
    }
    if (line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos) {
        return fail(TransportErrorCode::EmbeddedTerminator, "payload contains a CR or LF");
    }
    if (!is_valid_utf8(line)) {
        return fail(TransportErrorCode::InvalidUtf8, "payload is not well-formed UTF-8");
    }
    const std::size_t framed = line.size() + 1; // payload plus LF
    if (pending_.size() + framed > write_queue_bytes_) {
        // Make room first, so a QueueFull answer really means "no room now", not "the queue
        // happened to be stale".
        if (auto flushed = flush_once(); !flushed) {
            if (flushed.error().code == TransportErrorCode::WouldBlock) {
                return fail(TransportErrorCode::QueueFull, "stdio write queue is full");
            }
            return flushed;
        }
        if (pending_.size() + framed > write_queue_bytes_) {
            return fail(TransportErrorCode::QueueFull, "stdio write queue is full");
        }
    }
    pending_.append(line);
    pending_.push_back('\n');
    // A frame is accepted once it is queued: a WouldBlock here only means the OS buffer is full
    // right now, and the remainder will be sent by flush_pending() or the next write_line().
    if (auto flushed = flush_once();
        !flushed && flushed.error().code != TransportErrorCode::WouldBlock) {
        return flushed;
    }
    return {};
}

std::expected<void, TransportError> StdioTransport::flush_pending() {
    if (closed_) {
        return fail(TransportErrorCode::Closed, "stdio transport is closed");
    }
    return flush_once();
}

} // namespace tessera::harness
