// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_session.hpp"

#include <algorithm>

namespace tessera::ui::a11y {

std::expected<PumpResult, SessionError> AtspiSession::pump(int timeout_ms) {
    if (phase() == SessionPhase::Failed) {
        return std::unexpected(error());
    }
    PumpResult result;
    if (phase() == SessionPhase::Closed) {
        result.closed = true;
        result.idle = true;
        return result;
    }

    // Opening the connection happens inside the pump so `start()` never waits on I/O.
    if (!transport_) {
        const auto connected = connect_session_bus();
        if (!connected) {
            return std::unexpected(connected.error());
        }
    }

    const std::size_t read_budget =
        config_.max_reads_per_pump == 0 ? 1 : config_.max_reads_per_pump;
    for (std::size_t reads = 0; reads < read_budget; ++reads) {
        const auto read = read_once(timeout_ms, result);
        if (!read) {
            return std::unexpected(read.error());
        }
        if (!*read || result.closed) {
            break;
        }
    }

    if (!result.closed && phase() == SessionPhase::Ready) {
        result.events += bridge_.emit_events(out_);
    }
    if (!result.closed) {
        const auto written = flush();
        if (!written) {
            return std::unexpected(written.error());
        }
    }

    if (handshaking() && handshake_started_) {
        const auto budget = static_cast<std::uint64_t>(std::max(config_.handshake_timeout_ms, 0));
        if (steady_ms() - handshake_started_ms_ >= budget) {
            return std::unexpected(fail(SessionError::Timeout));
        }
    }

    result.idle = result.bytes_read == 0 && result.messages == 0;
    return result;
}

std::expected<bool, SessionError> AtspiSession::read_once(int timeout_ms, PumpResult& result) {
    if (!transport_) {
        return false;
    }
    const auto read = transport_->read_some(read_buffer_, timeout_ms);
    if (!read) {
        switch (read.error()) {
        case TransportError::WouldBlock:
            result.idle = true;
            return false;
        case TransportError::Closed:
            close_transport();
            if (phase() == SessionPhase::Ready) {
                // The registry went away after the handshake: that is a clean shutdown, not an
                // error, and the worker exits so `stop()` can join.
                phase_.store(SessionPhase::Closed);
                result.closed = true;
                return false;
            }
            return std::unexpected(fail(SessionError::TransportClosed));
        case TransportError::ConnectFailed:
            return std::unexpected(fail(SessionError::ConnectFailed));
        case TransportError::None:
            return false;
        case TransportError::NotConnected:
        case TransportError::IoError:
        case TransportError::TooLarge:
        case TransportError::Unsupported:
            return std::unexpected(fail(SessionError::TransportFailure));
        }
        return false;
    }
    if (*read == 0) {
        return false;
    }
    result.bytes_read += *read;
    const std::span<const std::uint8_t> bytes(read_buffer_.data(), *read);
    if (auth_) {
        const auto consumed = consume_auth(bytes, result);
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        return true;
    }
    if (in_.size() - in_offset_ + bytes.size() > kMaxWireMessage) {
        // A peer that never completes a message cannot make us buffer without bound.
        return std::unexpected(fail(SessionError::TransportFailure));
    }
    in_.insert(in_.end(), bytes.begin(), bytes.end());
    const auto consumed = consume_binary(result);
    if (!consumed) {
        return std::unexpected(consumed.error());
    }
    return true;
}

std::expected<void, SessionError> AtspiSession::flush() {
    if (out_.empty()) {
        return {};
    }
    if (!transport_) {
        out_.clear();
        return std::unexpected(fail(SessionError::TransportFailure));
    }
    const auto written = transport_->write_all(out_);
    out_.clear();
    if (!written) {
        if (written.error() == TransportError::Closed) {
            return std::unexpected(fail(SessionError::TransportClosed));
        }
        return std::unexpected(fail(SessionError::TransportFailure));
    }
    return {};
}

void AtspiSession::append_output(std::span<const std::uint8_t> bytes) {
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void AtspiSession::close_transport() {
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
    auth_.reset();
}

} // namespace tessera::ui::a11y
