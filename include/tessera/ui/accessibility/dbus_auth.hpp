// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::a11y {

// D-Bus SASL authentication, client side (docs/specs/accessibility-v1.md section 7). The AT-SPI2
// bus only needs the EXTERNAL mechanism: the client's identity is the hex-encoded decimal user id,
// and the bus daemon checks it against the socket credentials. No secret is ever transmitted.
//
// The handshake is a byte-level state machine so it can run on the bridge thread without blocking
// and be tested without a daemon:
//
//   client -> NUL "AUTH EXTERNAL <hex-uid>\r\n"
//   server -> "OK <guid>\r\n"  |  "REJECTED <mechanisms>\r\n"  |  "ERROR <text>\r\n"
//   client -> ("NEGOTIATE_UNIX_FD\r\n")  "BEGIN\r\n"
//
// After `BEGIN` the connection switches to the binary D-Bus protocol.
enum class AuthError : std::uint8_t {
    None = 0,
    IdentityTooLong, // the identity exceeds kMaxAuthIdentityBytes
    Rejected,        // the bus rejected the mechanism list
    ServerError,     // the bus answered ERROR
    Protocol,        // unexpected or malformed line for the current phase
    TooLarge,        // a server line exceeded kMaxAuthLineBytes
};

[[nodiscard]] std::string_view to_string(AuthError error);

enum class AuthPhase : std::uint8_t {
    Initial = 0,      // nothing accepted yet; the initial request has not been fed
    WaitingForOk,     // AUTH EXTERNAL sent
    WaitingForUnixFd, // NEGOTIATE_UNIX_FD sent
    Complete,         // BEGIN sent; the channel is now binary D-Bus
    Failed,
};

inline constexpr std::size_t kMaxAuthLineBytes = 4096;
inline constexpr std::size_t kMaxAuthIdentityBytes = 255;

// Hex-encodes the decimal text of `uid`, which is what the EXTERNAL mechanism expects
// (uid 1000 -> "31303030"). Not the raw uid bytes.
[[nodiscard]] std::string external_identity(std::uint32_t uid);

// The effective user id of the process, or 0 where the platform has no such notion (Windows).
[[nodiscard]] std::uint32_t current_uid();

class AuthClient {
public:
    explicit AuthClient(std::string identity, bool negotiate_unix_fd = false);

    // The bytes the client writes first: the NUL guard byte plus `AUTH EXTERNAL <identity>`.
    // Producing the request starts the handshake, so the next server line is expected to be `OK`.
    [[nodiscard]] std::expected<std::string, AuthError> initial_request();

    // Feeds raw bytes from the bus. Complete lines are handled in order, buffered partial lines are
    // kept for the next call, and every reply the client owes is appended to `out`. Returns the
    // phase after the input is consumed; a protocol failure is sticky.
    [[nodiscard]] std::expected<AuthPhase, AuthError> consume(std::span<const std::uint8_t> bytes,
                                                              std::vector<std::uint8_t>& out);

    // Handles one complete server line without its CRLF and appends the reply to `reply`.
    // `consume` is the normal entry point; this is exposed so tests can drive lines directly.
    [[nodiscard]] std::expected<AuthPhase, AuthError> handle_line(std::string_view line,
                                                                  std::string& reply);

    [[nodiscard]] AuthPhase phase() const {
        return phase_;
    }
    [[nodiscard]] AuthError error() const {
        return error_;
    }
    [[nodiscard]] std::string_view guid() const {
        return guid_;
    }
    // Mechanisms from a REJECTED line, for diagnostics.
    [[nodiscard]] std::string_view rejected_mechanisms() const {
        return mechanisms_;
    }
    // Text from an ERROR line, for diagnostics (never user content).
    [[nodiscard]] std::string_view server_error() const {
        return server_error_;
    }

private:
    [[nodiscard]] std::expected<AuthPhase, AuthError> fail(AuthError error);

    std::string identity_;
    std::string line_; // partial line between consume calls
    std::string guid_;
    std::string mechanisms_;
    std::string server_error_;
    bool negotiate_unix_fd_ = false;
    AuthPhase phase_ = AuthPhase::Initial;
    AuthError error_ = AuthError::None;
};

} // namespace tessera::ui::a11y
