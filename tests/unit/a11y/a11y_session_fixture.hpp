// SPDX-License-Identifier: MIT
//
// Shared harness for the `AtspiSession` tests: a loopback transport provider that keeps the peer
// endpoints, byte helpers that encode the daemon's replies, and stream readers. Every test drives
// the session with replayed bytes, so nothing here needs a bus daemon, a socket, or a wall clock.

#pragma once

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/atspi_bridge.hpp"
#include "tessera/ui/accessibility/atspi_session.hpp"
#include "tessera/ui/accessibility/dbus_auth.hpp"
#include "tessera/ui/accessibility/dbus_transport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a11y_session_test {

using tessera::ui::a11y::AtspiBridge;
using tessera::ui::a11y::AtspiSession;
using tessera::ui::a11y::AuthClient;
using tessera::ui::a11y::BusAddress;
using tessera::ui::a11y::current_uid;
using tessera::ui::a11y::external_identity;
using tessera::ui::a11y::LoopbackTransport;
using tessera::ui::a11y::MessageHeader;
using tessera::ui::a11y::MessageReader;
using tessera::ui::a11y::MessageType;
using tessera::ui::a11y::MessageWriter;
using tessera::ui::a11y::parse_message;
using tessera::ui::a11y::ParsedMessage;
using tessera::ui::a11y::SessionConfig;
using tessera::ui::a11y::SessionError;
using tessera::ui::a11y::SessionPhase;
using tessera::ui::a11y::Transport;
using tessera::ui::a11y::TransportProvider;

// The session address used by the replay: the loopback transport ignores it, but it has to parse so
// the state machine takes the normal path.
inline constexpr std::string_view kSessionAddressText = "unix:path=/tmp/tessera-test-session";
inline constexpr std::string_view kA11yAddressText = "unix:path=/tmp/tessera-test-a11y";

// Creates loopback pairs and keeps the peer half, so a test can inspect what the session wrote and
// feed what the daemon would reply.
class LoopbackProvider final : public TransportProvider {
public:
    std::unique_ptr<Transport> create() override {
        auto pair = LoopbackTransport::create();
        peers_.push_back(std::move(pair.second));
        return std::move(pair.first);
    }

    [[nodiscard]] std::size_t connection_count() const {
        return peers_.size();
    }
    LoopbackTransport& peer(std::size_t index) {
        return *peers_.at(index);
    }

private:
    std::vector<std::unique_ptr<LoopbackTransport>> peers_;
};

// Pumps until `read_output` has nothing left: the deterministic drain used after each replay step.
inline std::vector<std::uint8_t> read_output(LoopbackTransport& peer) {
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 8192> buffer{};
    for (;;) {
        const auto read = peer.read_some(buffer, 0);
        if (!read || *read == 0) {
            break;
        }
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
    }
    return out;
}

inline std::vector<std::uint8_t> concat(std::span<const std::uint8_t> first,
                                        std::span<const std::uint8_t> second) {
    std::vector<std::uint8_t> out(first.begin(), first.end());
    out.insert(out.end(), second.begin(), second.end());
    return out;
}

// Copies text into a bounded snapshot field (the node text arrays are not event detail arrays).
inline void set_node_text(std::array<char, tessera::ui::a11y::kMaxTextBytes>& target,
                          std::string_view text) {
    const std::size_t count = text.size() < target.size() ? text.size() : target.size() - 1;
    std::copy_n(text.begin(), count, target.begin());
    target[count] = '\0';
}

// The exact SASL request the session must send first; the uid is platform state, the encoding is
// the part the wire protocol fixes.
inline std::string expected_auth_request() {
    return std::string("\0", 1) + "AUTH EXTERNAL " + external_identity(current_uid()) + "\r\n";
}

// Encodes a MethodCall exactly as the session must emit it. The test owns this encoding so a byte
// comparison catches a wrong serial, destination, path, member, or signature.
template<typename Fn>
std::vector<std::uint8_t> encode_call(std::uint32_t serial, std::string_view destination,
                                      std::string_view path, std::string_view interface,
                                      std::string_view member, std::string_view signature,
                                      Fn&& body) {
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::MethodCall;
    header.serial = serial;
    header.destination = std::string(destination);
    header.path = std::string(path);
    header.interface = std::string(interface);
    header.member = std::string(member);
    header.signature = std::string(signature);
    (void)writer.begin_message(header);
    body(writer);
    (void)writer.end_message();
    return std::vector<std::uint8_t>(writer.bytes().begin(), writer.bytes().end());
}

// The daemon's reply. `body` writes the return values for `signature`.
template<typename Fn>
std::vector<std::uint8_t> encode_return(std::uint32_t reply_serial, std::string_view signature,
                                        Fn&& body) {
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::MethodReturn;
    header.serial = 1; // the daemon's own serial; the client only correlates reply_serial
    header.reply_serial = reply_serial;
    header.sender = "org.freedesktop.DBus";
    header.signature = std::string(signature);
    (void)writer.begin_message(header);
    body(writer);
    (void)writer.end_message();
    return std::vector<std::uint8_t>(writer.bytes().begin(), writer.bytes().end());
}

inline std::vector<std::uint8_t> string_return(std::uint32_t reply_serial, std::string_view text) {
    return encode_return(reply_serial, "s",
                         [text](MessageWriter& writer) { writer.write_string(text); });
}

inline std::vector<std::uint8_t> empty_return(std::uint32_t reply_serial) {
    return encode_return(reply_serial, std::string_view{}, [](MessageWriter&) {});
}

// Splits a stream into parsed messages; `bytes` must outlive the returned views.
inline std::vector<ParsedMessage> split_messages(std::span<const std::uint8_t> bytes) {
    std::vector<ParsedMessage> messages;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto parsed = parse_message(bytes.subspan(offset));
        if (!parsed) {
            break;
        }
        messages.push_back(*parsed);
        offset += parsed->consumed;
    }
    return messages;
}

inline bool expect_message(const ParsedMessage& message, MessageType type, std::string_view path,
                           std::string_view interface, std::string_view member,
                           std::string_view destination, std::string_view signature) {
    return a11y_dbus_test::expect(message.header.type == type, "message type") &&
           a11y_dbus_test::expect_equal(message.header.path, path, "message path") &&
           a11y_dbus_test::expect_equal(message.header.interface, interface, "message interface") &&
           a11y_dbus_test::expect_equal(message.header.member, member, "message member") &&
           a11y_dbus_test::expect_equal(message.header.destination, destination,
                                        "message destination") &&
           a11y_dbus_test::expect_equal(message.header.signature, signature, "message signature");
}

// Pumps once and reports the typed failure (with the phase it happened in) when the step fails.
inline bool pump_ok(AtspiSession& session, int timeout_ms, const char* what) {
    const auto result = session.pump(timeout_ms);
    if (result) {
        return true;
    }
    std::cerr << "a11y session: " << what << " failed ("
              << tessera::ui::a11y::to_string(result.error()) << ", phase "
              << tessera::ui::a11y::to_string(session.phase()) << ")\n";
    return false;
}

// The worker-thread variant: requests one worker pump and waits for it.
inline bool pump_sync_ok(AtspiSession& session, int timeout_ms, const char* what) {
    const auto result = session.pump_sync(timeout_ms);
    if (result) {
        return true;
    }
    std::cerr << "a11y session: " << what << " failed ("
              << tessera::ui::a11y::to_string(result.error()) << ", phase "
              << tessera::ui::a11y::to_string(session.phase()) << ")\n";
    return false;
}

// A bridge plus a session over a single loopback provider. The config points the session at the
// loopback address text and disables the environment reads.
struct Harness {
    AtspiBridge bridge;
    LoopbackProvider provider;
    AtspiSession session;

    Harness()
        : session(bridge, provider, make_config()) {}
    explicit Harness(SessionConfig config)
        : session(bridge, provider, std::move(config)) {}

    static SessionConfig make_config() {
        SessionConfig config;
        config.environment.session_bus_address = std::string(kSessionAddressText);
        config.environment.runtime_dir = "/tmp";
        return config;
    }
};

} // namespace a11y_session_test
