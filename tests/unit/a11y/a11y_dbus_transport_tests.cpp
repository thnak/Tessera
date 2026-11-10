// SPDX-License-Identifier: MIT
//
// Transport seam: the deterministic loopback pair the client is tested against, plus the platform
// factory contract (POSIX sockets exist on Linux only).

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/dbus_transport.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using tessera::ui::a11y::BusAddress;
using tessera::ui::a11y::BusTransport;
using tessera::ui::a11y::create_unix_socket_transport;
using tessera::ui::a11y::kMaxLoopbackBytes;
using tessera::ui::a11y::LoopbackTransport;
using tessera::ui::a11y::Transport;
using tessera::ui::a11y::TransportError;

struct Pair {
    std::unique_ptr<LoopbackTransport> client;
    std::unique_ptr<LoopbackTransport> peer;
};

Pair make_pair() {
    auto pair = LoopbackTransport::create();
    return Pair{std::move(pair.first), std::move(pair.second)};
}

bool loopback_delivers_bytes() {
    Pair pair = make_pair();
    const auto connected = pair.client->connect(BusAddress{BusTransport::UnixPath, "/tmp/bus"});
    if (!expect(connected.has_value(), "loopback connects") ||
        !expect(pair.client->connected(), "client reports connected")) {
        return false;
    }
    const std::vector<std::uint8_t> request = bytes_of("AUTH EXTERNAL 30\r\n");
    if (!expect(pair.client->write_all(request).has_value(), "client writes")) {
        return false;
    }
    std::array<std::uint8_t, 64> buffer{};
    const auto read = pair.peer->read_some(buffer, 0);
    if (!expect(read.has_value(), "peer reads") ||
        !expect(*read == request.size(), "peer reads it all") ||
        !expect_equal(text_of(std::span<const std::uint8_t>(buffer.data(), *read)),
                      "AUTH EXTERNAL 30\r\n", "peer sees the exact bytes")) {
        return false;
    }
    // The write is captured for golden assertions too.
    if (!expect_equal(text_of(pair.client->take_written()), "AUTH EXTERNAL 30\r\n",
                      "written bytes are captured") ||
        !expect(pair.client->take_written().empty(), "capture drains")) {
        return false;
    }
    return true;
}

bool loopback_partial_reads() {
    Pair pair = make_pair();
    pair.client->feed(bytes_of("0123456789"));
    std::array<std::uint8_t, 4> buffer{};
    if (!expect(pair.client->read_some(buffer, 0).value_or(0) == 4,
                "first chunk is size-bounded") ||
        !expect_equal(text_of(std::span<const std::uint8_t>(buffer.data(), 4)), "0123",
                      "first chunk")) {
        return false;
    }
    std::array<std::uint8_t, 32> rest{};
    const auto read = pair.client->read_some(rest, 0);
    return expect(read.has_value(), "second read succeeds") &&
           expect(*read == 6, "remaining bytes") &&
           expect_equal(text_of(std::span<const std::uint8_t>(rest.data(), *read)), "456789",
                        "remaining bytes text");
}

bool loopback_empty_is_would_block() {
    Pair pair = make_pair();
    std::array<std::uint8_t, 8> buffer{};
    if (!expect(pair.client->read_some(buffer, 0).error() == TransportError::WouldBlock,
                "empty queue is a deterministic would-block")) {
        return false;
    }
    // An empty destination is not an error and never consumes input.
    pair.client->feed(bytes_of("x"));
    return expect(pair.client->read_some({}, 0).value_or(99) == 0,
                  "empty destination reads zero") &&
           expect(pair.client->pending() == 1, "empty destination keeps the queue");
}

bool loopback_close_semantics() {
    Pair pair = make_pair();
    pair.client->feed(bytes_of("tail"));
    pair.peer->close();
    std::array<std::uint8_t, 8> buffer{};
    if (!expect(pair.client->read_some(buffer, 0).value_or(0) == 4,
                "queued bytes survive the close")) {
        return false;
    }
    if (!expect(pair.client->read_some(buffer, 0).error() == TransportError::Closed,
                "a closed peer reports closed once drained")) {
        return false;
    }
    if (!expect(pair.client->write_all(bytes_of("x")).error() == TransportError::Closed,
                "writing to a closed peer fails")) {
        return false;
    }
    pair.client->close();
    return expect(!pair.client->connected(), "close clears the connected flag") &&
           expect(pair.client->connect(BusAddress{BusTransport::UnixPath, "/tmp/bus"}).error() ==
                      TransportError::Closed,
                  "a closed endpoint cannot reconnect");
}

bool loopback_is_bounded() {
    Pair pair = make_pair();
    std::vector<std::uint8_t> oversized(kMaxLoopbackBytes + 1, 0);
    if (!expect(pair.client->write_all(oversized).error() == TransportError::TooLarge,
                "oversized write is rejected")) {
        return false;
    }
    std::vector<std::uint8_t> exact(kMaxLoopbackBytes, 0);
    if (!expect(pair.client->write_all(exact).has_value(), "write at the cap is accepted")) {
        return false;
    }
    return expect(pair.peer->pending() == kMaxLoopbackBytes, "queue is at the cap") &&
           expect(pair.client->write_all(bytes_of("x")).error() == TransportError::TooLarge,
                  "one more byte overflows");
}

bool loopback_shutdown() {
    Pair pair = make_pair();
    pair.client->shutdown();
    std::array<std::uint8_t, 4> buffer{};
    return expect(pair.peer->read_some(buffer, 0).error() == TransportError::Closed,
                  "shutdown closes both ends") &&
           expect(pair.client->read_some(buffer, 0).error() == TransportError::Closed,
                  "shutdown closes the initiator");
}

bool platform_factory_contract() {
#if defined(__linux__)
    const std::unique_ptr<Transport> transport = create_unix_socket_transport();
    return expect(transport != nullptr, "linux provides a UNIX socket transport") &&
           expect(!transport->connected(), "a fresh socket transport is not connected") &&
           expect(transport->write_all(bytes_of("x")).error() == TransportError::NotConnected,
                  "writes before connect fail") &&
           expect(transport->read_some(std::span<std::uint8_t>(), 0).value_or(99) == 0,
                  "empty reads do not need a connection");
#else
    return expect(create_unix_socket_transport() == nullptr,
                  "non-linux builds report no UNIX socket transport");
#endif
}

} // namespace

int main() {
    if (!loopback_delivers_bytes()) {
        return 1;
    }
    if (!loopback_partial_reads()) {
        return 2;
    }
    if (!loopback_empty_is_would_block()) {
        return 3;
    }
    if (!loopback_close_semantics()) {
        return 4;
    }
    if (!loopback_is_bounded()) {
        return 5;
    }
    if (!loopback_shutdown()) {
        return 6;
    }
    if (!platform_factory_contract()) {
        return 7;
    }
    std::cout << "a11y_dbus_transport: ok\n";
    return 0;
}
