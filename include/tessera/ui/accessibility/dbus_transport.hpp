// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/dbus_address.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace tessera::ui::a11y {

// Transport seam for the AT-SPI2 D-Bus client (docs/specs/accessibility-v1.md section 7). The D-Bus
// client speaks bytes; this interface is the only place that knows about sockets. Two
// implementations exist:
//
//   * `LoopbackTransport` - deterministic in-process pair used by every unit test, so no test needs
//     a real bus daemon, a socket, or a wall clock.
//   * the POSIX UNIX-socket transport created by `create_unix_socket_transport()` (Linux only; it
//     returns nullptr where the platform has no implementation, and the bridge then reports
//     accessibility disabled).
//
// All calls are non-blocking in the UI-thread sense: `read_some` waits at most `timeout_ms`, and
// callers only ever run it on the bridge thread (ADR-021: never block the UI thread).
enum class TransportError : std::uint8_t {
    None = 0,
    NotConnected,
    ConnectFailed,
    Closed,     // the peer closed the connection
    WouldBlock, // no bytes became available within the timeout
    IoError,
    TooLarge, // a write exceeded the transport's bounded buffer
    Unsupported,
};

class Transport {
public:
    virtual ~Transport() = default;

    // Establishes the connection. A transport that is already connected returns success.
    [[nodiscard]] virtual std::expected<void, TransportError>
    connect(const BusAddress& address) = 0;

    // Reads at most `out.size()` bytes, waiting up to `timeout_ms` for the first byte.
    // Returns 0 only for an empty destination span.
    [[nodiscard]] virtual std::expected<std::size_t, TransportError>
    read_some(std::span<std::uint8_t> out, int timeout_ms) = 0;

    // Writes the whole span or fails; partial writes are retried internally.
    [[nodiscard]] virtual std::expected<void, TransportError>
    write_all(std::span<const std::uint8_t> bytes) = 0;

    virtual void close() = 0;
    [[nodiscard]] virtual bool connected() const = 0;
};

// Deterministic in-process transport. `create()` returns a connected pair; bytes written to one
// endpoint are readable from the other. Nothing sleeps: `read_some` returns
// `TransportError::WouldBlock` immediately when the queue is empty, which keeps the bridge's
// poll/pump state machine fully deterministic under test.
inline constexpr std::size_t kMaxLoopbackBytes = 1024 * 1024;

class LoopbackTransport final : public Transport {
public:
    // Creates a connected pair. Index 0 is conventionally the client under test, index 1 the peer
    // that stands in for the bus.
    static std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>>
    create();

    [[nodiscard]] std::expected<void, TransportError> connect(const BusAddress& address) override;
    [[nodiscard]] std::expected<std::size_t, TransportError> read_some(std::span<std::uint8_t> out,
                                                                       int timeout_ms) override;
    [[nodiscard]] std::expected<void, TransportError>
    write_all(std::span<const std::uint8_t> bytes) override;
    void close() override;
    [[nodiscard]] bool connected() const override;

    // Test helpers. `take_written` drains the bytes this endpoint wrote; `feed` queues bytes for
    // this endpoint to read; `pending` is the readable queue depth.
    [[nodiscard]] std::vector<std::uint8_t> take_written();
    void feed(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::size_t pending() const;
    // Closes the connection from both sides so reads report `Closed` once the queue drains.
    void shutdown();

private:
    struct Pipe;
    explicit LoopbackTransport(std::shared_ptr<Pipe> pipe, std::size_t endpoint);

    std::shared_ptr<Pipe> pipe_;
    std::size_t endpoint_ = 0;
};

// POSIX UNIX-socket transport for `unix:path=` and `unix:abstract=` endpoints. Returns nullptr when
// the platform has no implementation (everything except Linux today); the bridge treats that as
// "accessibility unavailable" and keeps the app running.
[[nodiscard]] std::unique_ptr<Transport> create_unix_socket_transport();

} // namespace tessera::ui::a11y
