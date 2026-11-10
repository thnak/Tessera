// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/atspi_bridge.hpp"
#include "tessera/ui/accessibility/dbus_address.hpp"
#include "tessera/ui/accessibility/dbus_auth.hpp"
#include "tessera/ui/accessibility/dbus_transport.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tessera::ui::a11y {

// AT-SPI2 session handshake and pumped worker thread (docs/specs/accessibility-v1.md section 12.1).
// `AtspiSession` owns the two D-Bus connections the Linux bridge needs and runs the handshake an
// AT-SPI2 client must perform before it can serve anything:
//
//   session bus -> connect -> SASL EXTERNAL -> `org.freedesktop.DBus.Hello`
//               -> `org.a11y.Bus.GetAddress` at `/org/a11y/bus`
//   a11y bus    -> connect -> SASL EXTERNAL -> `Hello`
//               -> `org.a11y.atspi.Registry.RegisterEvent` for the object events the bridge emits
//
// Every step is a state machine over bytes the transport already delivered, so a step never blocks
// waiting for a reply that has not arrived: `pump` reads within a fixed budget, frames complete
// messages, answers handshake replies, hands AT method calls to `AtspiBridge::dispatch`, and emits
// queued events. Nothing here throws; every failure is a `SessionError` value (ADR-021: no
// blocking on the UI thread, no detached work).
//
// Threading: `start()` owns exactly one worker thread and `stop()` joins it. The worker only calls
// `pump`, which is the one function that touches the transport. A caller that needs a deterministic
// single step (tests, a host with its own loop) either calls `pump` from the thread that owns the
// session, or `pump_sync` while the worker is running.

enum class SessionError : std::uint8_t {
    None = 0,
    Disabled,         // the platform has no UNIX-socket transport; accessibility stays off
    AddressMissing,   // neither DBUS_SESSION_BUS_ADDRESS nor a runtime directory was available
    AddressInvalid,   // an address string could not be resolved to a usable UNIX endpoint
    ConnectFailed,    // the socket connect failed
    AuthRejected,     // the bus refused SASL EXTERNAL
    AuthProtocol,     // the bus sent an unexpected SASL line
    AuthFailed,       // another SASL failure (identity too long, line too long)
    TransportClosed,  // the peer closed the connection before the handshake finished
    TransportFailure, // a read/write failed or a bounded buffer overflowed
    Protocol,         // a malformed or unexpected D-Bus message
    UnexpectedReply,  // a reply whose reply_serial matches no pending handshake call
    BusError,         // the bus answered a handshake call with a D-Bus Error message
    Timeout,          // the handshake did not finish within `handshake_timeout_ms`
    TooManyCalls,     // the bounded pending-call table overflowed (one call is outstanding at most)
    ThreadFailed,     // the worker thread could not be created
    NotRunning,       // `pump_sync` was called on a session whose worker is not running
    SyncTimeout,      // `pump_sync` waited past its liveness bound without a completed pump
};

[[nodiscard]] std::string_view to_string(SessionError error);

enum class SessionPhase : std::uint8_t {
    Idle = 0,       // nothing started
    SessionConnect, // connecting/serializing the session-bus connection
    SessionHello,   // `Hello` sent on the session bus, waiting for the unique name
    A11yAddress,    // `org.a11y.Bus.GetAddress` sent, waiting for the accessibility bus address
    A11yConnect,    // connecting/serializing the accessibility bus connection
    A11yHello,      // `Hello` sent on the accessibility bus
    Registering,    // `Registry.RegisterEvent` sent, waiting for the (empty) reply
    Ready,          // registered: serving method calls and emitting events
    Closed,         // the peer closed, or `stop()` finished
    Failed,         // terminal error; `error()` says which step failed
};

[[nodiscard]] std::string_view to_string(SessionPhase phase);

// The well-known names and object paths the handshake uses. Kept as constants so tests and
// diagnostics share one spelling with the state machine.
inline constexpr std::string_view kSessionBusName = "org.freedesktop.DBus";
inline constexpr std::string_view kSessionBusPath = "/org/freedesktop/DBus";
inline constexpr std::string_view kSessionBusInterface = "org.freedesktop.DBus";
inline constexpr std::string_view kSessionBusMember = "Hello";
inline constexpr std::string_view kA11yBusName = "org.a11y.Bus";
inline constexpr std::string_view kA11yBusPath = "/org/a11y/bus";
inline constexpr std::string_view kA11yBusMember = "GetAddress";
inline constexpr std::string_view kRegistryBusName = "org.a11y.atspi.Registry";
inline constexpr std::string_view kRegistryPath = "/org/a11y/atspi/registry";
inline constexpr std::string_view kRegistryInterface = "org.a11y.atspi.Registry";
inline constexpr std::string_view kRegistryMember = "RegisterEvent";

// The object events registered after the accessibility bus hello. They match the `EventKind`
// members `atspi_events.hpp` emits: state/property/bounds changes.
inline constexpr std::size_t kEventRegistrationCount = 3;
inline constexpr std::array<std::string_view, kEventRegistrationCount> kEventRegistrations{
    "object:state-changed", "object:property-change", "object:bounds-changed"};

// Creates the sockets a session connects through. The default provider is `UnixTransportProvider`
// (nullptr off Linux); tests inject a loopback provider so no test needs a daemon or a socket.
class TransportProvider {
public:
    virtual ~TransportProvider() = default;

    // Creates one unconnected transport, or nullptr when the platform has no implementation.
    [[nodiscard]] virtual std::unique_ptr<Transport> create() = 0;
};

class UnixTransportProvider final : public TransportProvider {
public:
    [[nodiscard]] std::unique_ptr<Transport> create() override;
};

struct SessionConfig {
    // Explicit inputs so tests never read the process environment;
    // `session_config_from_environment` is the one helper that copies them.
    SessionEnvironment environment;
    // Total budget for the handshake; 0 makes the next pump report `Timeout`, which is how the
    // timeout path stays deterministic.
    int handshake_timeout_ms = 5000;
    // Per-`read_some` wait and per-pump read count, so one pump can never run unbounded.
    int read_timeout_ms = 8;
    std::size_t max_reads_per_pump = 4;
    // Worker-thread idle wait between pumps when the peer is quiet (never a busy spin).
    int idle_wait_ms = 4;
    // Skip `Registry.RegisterEvent` when the host registers events itself.
    bool register_events = true;
};

// Reads DBUS_SESSION_BUS_ADDRESS/XDG_RUNTIME_DIR into an owned config. The only environment touch.
[[nodiscard]] SessionConfig session_config_from_environment();

// What one pump iteration did. `idle` means no bytes arrived and no message was handled, so a
// worker loop should wait before polling again. `closed` means the peer closed; the session is then
// `Closed` and the worker exits.
struct PumpResult {
    std::size_t bytes_read = 0;
    std::size_t messages = 0;
    std::size_t dispatched = 0;
    std::size_t events = 0;
    bool idle = false;
    bool closed = false;
};

class AtspiSession {
public:
    AtspiSession(AtspiBridge& bridge, TransportProvider& provider, SessionConfig config = {});
    explicit AtspiSession(AtspiBridge& bridge, SessionConfig config = {});
    ~AtspiSession();

    AtspiSession(const AtspiSession&) = delete;
    AtspiSession& operator=(const AtspiSession&) = delete;

    // Resets the handshake state and spawns the one worker thread. Idempotent while running.
    [[nodiscard]] std::expected<void, SessionError> start();
    // Signals the worker and joins it; safe to call twice, from the destructor, or when never
    // started. The transport is closed after the join.
    void stop();
    [[nodiscard]] bool running() const;

    // One bounded iteration: at most `max_reads_per_pump` reads, a fixed message budget, then one
    // batched write. Only the thread that owns the session may call it (the worker, or a host loop
    // before `start()`/after `stop()`).
    [[nodiscard]] std::expected<PumpResult, SessionError> pump(int timeout_ms);
    // Requests a worker iteration and waits until one finishes; `wait_ms` is a liveness bound, not
    // part of the protocol. Used by tests and by hosts that want a step-per-call driver.
    [[nodiscard]] std::expected<PumpResult, SessionError> pump_sync(int timeout_ms,
                                                                    int wait_ms = 5000);

    [[nodiscard]] SessionPhase phase() const;
    [[nodiscard]] SessionError error() const;
    [[nodiscard]] bool ready() const {
        return phase() == SessionPhase::Ready;
    }
    // Owning copies: the caller may read them while the worker keeps running.
    [[nodiscard]] std::string unique_name() const;
    [[nodiscard]] std::string a11y_bus_address() const;
    [[nodiscard]] std::string bus_error_name() const;
    // Completed pump iterations, for lifecycle tests and diagnostics.
    [[nodiscard]] std::size_t pumps() const;

private:
    enum class CallKind : std::uint8_t { Hello = 0, GetA11yAddress, RegisterEvent };
    struct PendingCall {
        std::uint32_t serial = 0;
        CallKind kind = CallKind::Hello;
    };

    static constexpr std::size_t kMaxPendingCalls = 4;
    static constexpr std::size_t kMaxMessagesPerPump = 16;
    static constexpr std::size_t kReadChunkBytes = 4096;

    // Connection + SASL.
    [[nodiscard]] std::expected<void, SessionError> connect_session_bus();
    [[nodiscard]] std::expected<void, SessionError> connect_a11y_bus(std::string address);
    [[nodiscard]] std::expected<void, SessionError> open_transport(const BusAddress& address);
    [[nodiscard]] std::expected<void, SessionError>
    consume_auth(std::span<const std::uint8_t> bytes, PumpResult& result);
    [[nodiscard]] std::expected<void, SessionError> start_binary_phase();

    // Binary protocol.
    [[nodiscard]] std::expected<std::size_t, SessionError> frame_size() const;
    [[nodiscard]] std::expected<void, SessionError> consume_binary(PumpResult& result);
    [[nodiscard]] std::expected<void, SessionError>
    handle_message(const ParsedMessage& message, std::span<const std::uint8_t> raw,
                   PumpResult& result);
    [[nodiscard]] std::expected<void, SessionError> handle_reply(const MessageHeader& header,
                                                                 std::span<const std::uint8_t> body,
                                                                 PumpResult& result);

    // Handshake calls: each writes one complete message into `out_` and records its serial.
    [[nodiscard]] std::expected<void, SessionError> send_hello();
    [[nodiscard]] std::expected<void, SessionError> send_get_address();
    [[nodiscard]] std::expected<void, SessionError> send_register_event();
    [[nodiscard]] std::expected<void, SessionError>
    send_call(CallKind kind, std::string_view destination, std::string_view path,
              std::string_view interface, std::string_view member, std::string_view signature);
    [[nodiscard]] PendingCall* find_call(std::uint32_t serial);
    void clear_calls();

    // I/O.
    [[nodiscard]] std::expected<bool, SessionError> read_once(int timeout_ms, PumpResult& result);
    [[nodiscard]] std::expected<void, SessionError> flush();
    void close_transport();
    void append_output(std::span<const std::uint8_t> bytes);

    // Lifecycle.
    [[nodiscard]] SessionError fail(SessionError error);
    void reset_state();
    void run_loop();
    bool wait_for_pump(std::uint64_t ticket, int wait_ms);
    [[nodiscard]] bool handshaking() const;
    // Monotonic milliseconds for the handshake budget; the session never reads a wall clock.
    [[nodiscard]] static std::uint64_t steady_ms();

    AtspiBridge& bridge_;
    std::unique_ptr<UnixTransportProvider> owned_provider_;
    TransportProvider* provider_ = nullptr;
    SessionConfig config_;

    std::unique_ptr<Transport> transport_;
    std::unique_ptr<AuthClient> auth_;
    MessageWriter writer_;
    std::vector<std::uint8_t> out_;
    std::vector<std::uint8_t> in_;
    std::size_t in_offset_ = 0;
    std::array<std::uint8_t, kReadChunkBytes> read_buffer_{};
    std::array<PendingCall, kMaxPendingCalls> calls_{};
    std::size_t call_count_ = 0;

    std::atomic<SessionPhase> phase_{SessionPhase::Idle};
    std::atomic<SessionError> error_{SessionError::None};
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> pumps_{0};
    bool on_a11y_bus_ = false;
    bool handshake_started_ = false;
    std::uint64_t handshake_started_ms_ = 0;

    mutable std::mutex state_mutex_;
    std::string unique_name_;
    std::string a11y_address_;
    std::string bus_error_name_;

    std::thread thread_;
    mutable std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::uint64_t wake_serial_ = 0;
    std::uint64_t completed_serial_ = 0;
    bool wake_pending_ = false;
    bool stop_requested_ = false;
    bool worker_busy_ = false;
    bool worker_done_ = false;
    int next_timeout_ms_ = -1;
    PumpResult last_pump_;
};

} // namespace tessera::ui::a11y
