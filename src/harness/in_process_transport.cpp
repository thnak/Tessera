// SPDX-License-Identifier: MIT

#include "tessera/harness/in_process_transport.hpp"
#include "tessera/harness/utf8.hpp"

#include <string>
#include <utility>

namespace tessera::harness {
namespace {

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

} // namespace

struct InProcessTransport::Shared {
    explicit Shared(std::size_t max_line_bytes)
        : max_line_bytes(max_line_bytes),
          first_inbound(max_line_bytes),
          second_inbound(max_line_bytes) {}

    std::size_t max_line_bytes;
    LineFramer first_inbound;  // bytes the first endpoint reads
    LineFramer second_inbound; // bytes the second endpoint reads
    bool first_closed = false;
    bool second_closed = false;
};

InProcessTransport::InProcessTransport(std::shared_ptr<Shared> shared, bool is_first) noexcept
    : shared_(std::move(shared)),
      is_first_(is_first) {}

InProcessTransport::~InProcessTransport() {
    close();
}

LineFramer& InProcessTransport::inbound() noexcept {
    return is_first_ ? shared_->first_inbound : shared_->second_inbound;
}

std::size_t InProcessTransport::max_line_bytes() const noexcept {
    return shared_->max_line_bytes;
}

bool InProcessTransport::closed() const noexcept {
    return closed_;
}

bool InProcessTransport::peer_closed() const noexcept {
    return is_first_ ? shared_->second_closed : shared_->first_closed;
}

void InProcessTransport::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (is_first_) {
        shared_->first_closed = true;
    } else {
        shared_->second_closed = true;
    }
}

std::expected<std::string, TransportError> InProcessTransport::read_line() {
    if (closed_) {
        return std::unexpected(TransportError{TransportErrorCode::Closed, "endpoint closed"});
    }
    LineFramer& framer = inbound();
    std::string line;
    for (;;) {
        FrameError frame_error;
        switch (framer.next(line, frame_error)) {
        case LineFramer::Status::Line:
            return line;
        case LineFramer::Status::Rejected:
            return std::unexpected(
                TransportError{to_transport_code(frame_error.code), describe(frame_error)});
        case LineFramer::Status::NeedMore:
            if (!peer_closed()) {
                return std::unexpected(
                    TransportError{TransportErrorCode::WouldBlock, "no complete line yet"});
            }
            // The peer is gone: an unterminated tail is still a frame, and finish() guarantees
            // the next call cannot report NeedMore again.
            framer.finish();
            break;
        case LineFramer::Status::EndOfStream:
            return std::unexpected(
                TransportError{TransportErrorCode::Closed, "peer closed the stream"});
        }
    }
}

std::expected<void, TransportError> InProcessTransport::write_line(std::string_view line) {
    if (closed_) {
        return std::unexpected(TransportError{TransportErrorCode::Closed, "endpoint closed"});
    }
    if (peer_closed()) {
        return std::unexpected(
            TransportError{TransportErrorCode::Closed, "peer closed the stream"});
    }
    if (line.empty()) {
        return std::unexpected(
            TransportError{TransportErrorCode::EmptyLine, "an empty line is not a frame"});
    }
    if (line.size() > shared_->max_line_bytes) {
        return std::unexpected(
            TransportError{TransportErrorCode::LineTooLong, "line exceeds the transport cap"});
    }
    if (line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos) {
        return std::unexpected(
            TransportError{TransportErrorCode::EmbeddedTerminator, "payload contains a CR or LF"});
    }
    if (!is_valid_utf8(line)) {
        return std::unexpected(
            TransportError{TransportErrorCode::InvalidUtf8, "payload is not well-formed UTF-8"});
    }
    LineFramer& peer_inbound = is_first_ ? shared_->second_inbound : shared_->first_inbound;
    peer_inbound.append(line);
    peer_inbound.append("\n");
    return {};
}

InProcessPair make_in_process_pair(std::size_t max_line_bytes) {
    auto shared = std::make_shared<InProcessTransport::Shared>(max_line_bytes);
    InProcessPair pair;
    pair.first = std::unique_ptr<InProcessTransport>(new InProcessTransport(shared, true));
    pair.second =
        std::unique_ptr<InProcessTransport>(new InProcessTransport(std::move(shared), false));
    return pair;
}

} // namespace tessera::harness
