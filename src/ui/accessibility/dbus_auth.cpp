// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/dbus_auth.hpp"

#include <utility>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace tessera::ui::a11y {
namespace {

char hex_digit(unsigned int value) {
    return static_cast<char>(value < 10U ? '0' + value : 'A' + (value - 10U));
}

// Text after the first space of a response line, or empty when the line is only the verb.
std::string_view argument_of(std::string_view line) {
    const std::size_t space = line.find(' ');
    return space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
}

bool is_verb(std::string_view line, std::string_view verb) {
    return line == verb ||
           (line.size() > verb.size() && line.starts_with(verb) && line[verb.size()] == ' ');
}

} // namespace

std::string_view to_string(AuthError error) {
    switch (error) {
    case AuthError::None:
        return "none";
    case AuthError::IdentityTooLong:
        return "identity too long";
    case AuthError::Rejected:
        return "rejected";
    case AuthError::ServerError:
        return "server error";
    case AuthError::Protocol:
        return "protocol error";
    case AuthError::TooLarge:
        return "line too large";
    }
    return "unknown";
}

std::string external_identity(std::uint32_t uid) {
    const std::string text = std::to_string(uid);
    std::string hex;
    hex.reserve(text.size() * 2);
    for (const char digit : text) {
        const auto byte = static_cast<unsigned int>(static_cast<unsigned char>(digit));
        hex.push_back(hex_digit(byte >> 4));
        hex.push_back(hex_digit(byte & 0x0FU));
    }
    return hex;
}

std::uint32_t current_uid() {
#if defined(__linux__)
    return static_cast<std::uint32_t>(::geteuid());
#else
    return 0;
#endif
}

AuthClient::AuthClient(std::string identity, bool negotiate_unix_fd)
    : identity_(std::move(identity)),
      negotiate_unix_fd_(negotiate_unix_fd) {
    if (identity_.size() > kMaxAuthIdentityBytes) {
        phase_ = AuthPhase::Failed;
        error_ = AuthError::IdentityTooLong;
    }
}

std::expected<std::string, AuthError> AuthClient::initial_request() {
    if (error_ == AuthError::IdentityTooLong) {
        return std::unexpected(AuthError::IdentityTooLong);
    }
    std::string request;
    request.reserve(identity_.size() + 20);
    request.push_back('\0');
    request.append("AUTH EXTERNAL ");
    request.append(identity_);
    request.append("\r\n");
    if (phase_ == AuthPhase::Initial) {
        phase_ = AuthPhase::WaitingForOk;
    }
    return request;
}

std::expected<AuthPhase, AuthError> AuthClient::fail(AuthError error) {
    phase_ = AuthPhase::Failed;
    error_ = error;
    return std::unexpected(error_);
}

std::expected<AuthPhase, AuthError> AuthClient::handle_line(std::string_view line,
                                                            std::string& reply) {
    if (phase_ == AuthPhase::Failed) {
        return std::unexpected(error_);
    }
    if (phase_ == AuthPhase::Complete) {
        // The channel is binary now; anything that still looks like a SASL line is a protocol
        // error.
        return fail(AuthError::Protocol);
    }
    if (phase_ == AuthPhase::Initial) {
        // The client must have sent AUTH before the bus can answer.
        return fail(AuthError::Protocol);
    }
    if (line.empty()) {
        return fail(AuthError::Protocol);
    }

    if (is_verb(line, "OK")) {
        if (phase_ != AuthPhase::WaitingForOk) {
            return fail(AuthError::Protocol);
        }
        guid_ = std::string(argument_of(line));
        if (guid_.empty()) {
            return fail(AuthError::Protocol);
        }
        if (negotiate_unix_fd_) {
            phase_ = AuthPhase::WaitingForUnixFd;
            reply.append("NEGOTIATE_UNIX_FD\r\n");
            return phase_;
        }
        phase_ = AuthPhase::Complete;
        reply.append("BEGIN\r\n");
        return phase_;
    }
    if (is_verb(line, "REJECTED")) {
        mechanisms_ = std::string(argument_of(line));
        return fail(AuthError::Rejected);
    }
    if (is_verb(line, "DATA")) {
        // EXTERNAL sent its whole identity up front; a request for more data means the bus does not
        // accept the mechanism. ERROR is the specified way to decline.
        reply.append("ERROR\r\n");
        return fail(AuthError::Protocol);
    }
    if (is_verb(line, "ERROR")) {
        server_error_ = std::string(argument_of(line));
        if (phase_ == AuthPhase::WaitingForUnixFd) {
            // fd passing is an optional upgrade: a refusal means a plain connection still works, and
            // the bridge never needs file descriptors for AT-SPI2 MethodCall/Reply traffic.
            phase_ = AuthPhase::Complete;
            reply.append("BEGIN\r\n");
            return phase_;
        }
        return fail(AuthError::ServerError);
    }
    if (is_verb(line, "AGREE_UNIX_FD")) {
        if (phase_ != AuthPhase::WaitingForUnixFd) {
            return fail(AuthError::Protocol);
        }
        phase_ = AuthPhase::Complete;
        reply.append("BEGIN\r\n");
        return phase_;
    }
    return fail(AuthError::Protocol);
}

std::expected<AuthPhase, AuthError> AuthClient::consume(std::span<const std::uint8_t> bytes,
                                                        std::vector<std::uint8_t>& out) {
    if (phase_ == AuthPhase::Failed) {
        return std::unexpected(error_);
    }
    for (const std::uint8_t byte : bytes) {
        if (line_.size() >= kMaxAuthLineBytes) {
            return fail(AuthError::TooLarge);
        }
        line_.push_back(static_cast<char>(byte));
        if (byte != '\n') {
            continue;
        }
        if (!line_.empty() && line_.back() == '\n') {
            line_.pop_back();
        }
        if (!line_.empty() && line_.back() == '\r') {
            line_.pop_back();
        }
        std::string reply;
        const auto step = handle_line(line_, reply);
        line_.clear();
        out.insert(out.end(), reply.begin(), reply.end());
        if (!step) {
            return step;
        }
    }
    return phase_;
}

} // namespace tessera::ui::a11y
