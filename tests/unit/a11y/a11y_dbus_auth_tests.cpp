// SPDX-License-Identifier: MIT
//
// SASL EXTERNAL handshake: initial request bytes, OK/REJECTED/ERROR/DATA handling, the optional
// UNIX-fd negotiation, partial-line buffering, and the bounded line cap.

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/dbus_auth.hpp"

#include <string>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using tessera::ui::a11y::AuthClient;
using tessera::ui::a11y::AuthError;
using tessera::ui::a11y::AuthPhase;
using tessera::ui::a11y::current_uid;
using tessera::ui::a11y::external_identity;
using tessera::ui::a11y::kMaxAuthLineBytes;
using tessera::ui::a11y::to_string;

std::string feed(AuthClient& client, std::string_view text) {
    std::vector<std::uint8_t> out;
    (void)client.consume(bytes_of(text), out);
    return text_of(out);
}

bool identity_encoding() {
    // The uid itself is platform state (0 on Windows, the effective uid on Linux); the encoding is
    // what the wire protocol needs to be exact about.
    (void)current_uid();
    return expect_equal(external_identity(1000), "31303030", "uid 1000 is hex-encoded ASCII") &&
           expect_equal(external_identity(0), "30", "uid 0") &&
           expect_equal(external_identity(65534), "3635353334", "nobody uid");
}

bool initial_request_bytes() {
    AuthClient client(external_identity(1000));
    const auto request = client.initial_request();
    if (!expect(request.has_value(), "initial request exists")) {
        return false;
    }
    const std::string expected = std::string("\0", 1) + "AUTH EXTERNAL 31303030\r\n";
    return expect_equal(*request, expected, "initial request bytes") &&
           expect(client.phase() == AuthPhase::WaitingForOk, "request starts the handshake");
}

bool ok_completes() {
    AuthClient client(external_identity(1000));
    (void)client.initial_request();
    const std::string reply = feed(client, "OK 0123456789abcdef\r\n");
    return expect_equal(reply, "BEGIN\r\n", "OK is answered with BEGIN") &&
           expect(client.phase() == AuthPhase::Complete, "OK completes the handshake") &&
           expect_equal(client.guid(), "0123456789abcdef", "server guid is captured");
}

bool unix_fd_negotiation() {
    AuthClient client(external_identity(1000), true);
    (void)client.initial_request();
    const std::string negotiate = feed(client, "OK abc\r\n");
    if (!expect_equal(negotiate, "NEGOTIATE_UNIX_FD\r\n", "fd negotiation is requested") ||
        !expect(client.phase() == AuthPhase::WaitingForUnixFd, "waiting for the fd answer")) {
        return false;
    }
    const std::string begin = feed(client, "AGREE_UNIX_FD\r\n");
    if (!expect_equal(begin, "BEGIN\r\n", "agreement is answered with BEGIN") ||
        !expect(client.phase() == AuthPhase::Complete, "fd negotiation completes")) {
        return false;
    }
    // A bus that refuses fd passing still completes the handshake.
    AuthClient refusing(external_identity(1000), true);
    (void)refusing.initial_request();
    (void)feed(refusing, "OK abc\r\n");
    const std::string fallback = feed(refusing, "ERROR fd passing disabled\r\n");
    return expect_equal(fallback, "BEGIN\r\n", "refused fd negotiation still begins") &&
           expect(refusing.phase() == AuthPhase::Complete, "refusal completes the handshake");
}

bool rejections() {
    {
        AuthClient client(external_identity(1000));
        (void)client.initial_request();
        const std::string reply = feed(client, "REJECTED EXTERNAL DBUS_COOKIE_SHA1\r\n");
        if (!expect(reply.empty(), "a rejection sends nothing") ||
            !expect(client.phase() == AuthPhase::Failed, "rejection fails the handshake") ||
            !expect(client.error() == AuthError::Rejected, "rejection is classified") ||
            !expect_equal(client.rejected_mechanisms(), "EXTERNAL DBUS_COOKIE_SHA1",
                          "mechanisms are captured")) {
            return false;
        }
        // Failure is sticky: later bytes do not restart the machine.
        std::vector<std::uint8_t> out;
        const auto after = client.consume(bytes_of("OK abc\r\n"), out);
        return expect(!after.has_value() && after.error() == AuthError::Rejected,
                      "failure is sticky");
    }
}

bool server_errors() {
    AuthClient client(external_identity(1000));
    (void)client.initial_request();
    const std::string reply = feed(client, "ERROR no EXTERNAL on this socket\r\n");
    return expect(reply.empty(), "ERROR sends nothing") &&
           expect(client.error() == AuthError::ServerError, "ERROR is classified") &&
           expect_equal(client.server_error(), "no EXTERNAL on this socket", "error text") &&
           expect_equal(to_string(AuthError::ServerError), "server error", "error name");
}

bool data_is_declined() {
    AuthClient client(external_identity(1000));
    (void)client.initial_request();
    const std::string reply = feed(client, "DATA\r\n");
    return expect_equal(reply, "ERROR\r\n", "extra DATA is declined") &&
           expect(client.error() == AuthError::Protocol,
                  "DATA after the identity is a protocol error");
}

bool unexpected_lines() {
    {
        AuthClient client(external_identity(1000));
        (void)client.initial_request();
        const std::string reply = feed(client, "AGREE_UNIX_FD\r\n");
        if (!expect(reply.empty(), "unsolicited agreement sends nothing") ||
            !expect(client.error() == AuthError::Protocol,
                    "unsolicited agreement is a protocol error")) {
            return false;
        }
    }
    {
        AuthClient client(external_identity(1000));
        (void)client.initial_request();
        std::string reply;
        if (!expect(client.handle_line("WHATEVER", reply).error() == AuthError::Protocol,
                    "unknown verb")) {
            return false;
        }
    }
    {
        // A line before the initial request was produced means the client is out of order.
        AuthClient client(external_identity(1000));
        std::string reply;
        if (!expect(client.handle_line("OK abc", reply).error() == AuthError::Protocol,
                    "line before AUTH")) {
            return false;
        }
    }
    {
        AuthClient client(external_identity(1000));
        (void)client.initial_request();
        (void)feed(client, "OK abc\r\n");
        std::string reply;
        return expect(client.handle_line("OK again", reply).error() == AuthError::Protocol,
                      "lines after BEGIN are protocol errors");
    }
}

bool partial_lines() {
    AuthClient client(external_identity(1000));
    (void)client.initial_request();
    if (!expect(feed(client, "O").empty(), "partial verb produces no reply") ||
        !expect(feed(client, "K abc").empty(), "partial line produces no reply")) {
        return false;
    }
    const std::string completed = feed(client, "\r\n");
    return expect_equal(completed, "BEGIN\r\n", "CRLF completes the buffered line") &&
           expect(client.phase() == AuthPhase::Complete, "buffered line completes the handshake");
}

bool line_bound() {
    AuthClient client(external_identity(1000));
    (void)client.initial_request();
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> oversized(kMaxAuthLineBytes + 1, 'a');
    const auto result = client.consume(oversized, out);
    return expect(!result.has_value(), "oversized line fails") &&
           expect(result.error() == AuthError::TooLarge, "oversized line is classified") &&
           expect(out.empty(), "oversized line sends nothing");
}

bool identity_bound() {
    AuthClient client(std::string(300, 'a'));
    const auto request = client.initial_request();
    return expect(!request.has_value(), "over-long identity is rejected") &&
           expect(request.error() == AuthError::IdentityTooLong, "identity error is classified") &&
           expect(client.phase() == AuthPhase::Failed, "identity failure is immediate");
}

} // namespace

int main() {
    if (!identity_encoding()) {
        return 1;
    }
    if (!initial_request_bytes()) {
        return 2;
    }
    if (!ok_completes()) {
        return 3;
    }
    if (!unix_fd_negotiation()) {
        return 4;
    }
    if (!rejections()) {
        return 5;
    }
    if (!server_errors()) {
        return 6;
    }
    if (!data_is_declined()) {
        return 7;
    }
    if (!unexpected_lines()) {
        return 8;
    }
    if (!partial_lines()) {
        return 9;
    }
    if (!line_bound()) {
        return 10;
    }
    if (!identity_bound()) {
        return 11;
    }
    std::cout << "a11y_dbus_auth: ok\n";
    return 0;
}
