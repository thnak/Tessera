// SPDX-License-Identifier: MIT
//
// `AtspiSession` worker lifecycle: `start()` owns one thread, `stop()` joins it, the handshake
// completes through `pump_sync` barriers, and the error paths (handshake timeout, peer close before
// and after Ready) are values. No test needs a real socket or a bus daemon.

#include "a11y_session_fixture.hpp"
#include "tessera/ui/accessibility/atspi_session.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using namespace a11y_session_test;

using ByteStream = std::vector<std::uint8_t>;
using ByteLog = std::vector<ByteStream>;

bool expect_phase(const AtspiSession& session, SessionPhase phase, const char* message) {
    return expect(session.phase() == phase, message);
}

// Runs the full handshake through the worker thread. Each feed is followed by a `pump_sync`
// barrier, so the pump that observes the bytes completes before the next assertion.
bool drive_worker(Harness& harness, ByteLog* log = nullptr) {
    auto record = [log](const ByteStream& bytes) {
        if (log != nullptr) {
            log->push_back(bytes);
        }
        return bytes;
    };
    AtspiSession& session = harness.session;
    if (!expect(session.start().has_value(), "start spawns the worker") ||
        !expect(session.running(), "worker reports running")) {
        return false;
    }
    if (!pump_sync_ok(session, 0, "connect pump")) {
        return false;
    }
    if (!expect_equal(hex(record(read_output(harness.provider.peer(0)))),
                      hex(expected_auth_request()), "worker sends the SASL request")) {
        return false;
    }

    (void)harness.provider.peer(0).write_all(bytes_of("OK 0123456789abcdef\r\n"));
    if (!pump_sync_ok(session, 0, "auth pump") ||
        !expect_phase(session, SessionPhase::SessionHello, "session-hello after auth")) {
        return false;
    }
    {
        const ByteStream bytes = record(read_output(harness.provider.peer(0)));
        if (!expect(bytes.size() > 7, "BEGIN plus a message")) {
            return false;
        }
        const auto messages = split_messages(std::span(bytes).subspan(7));
        if (!expect(messages.size() == 1, "one Hello message") ||
            !expect_equal(messages[0].header.member, "Hello", "Hello member") ||
            !expect(messages[0].header.serial == 1, "Hello serial")) {
            return false;
        }
    }

    (void)harness.provider.peer(0).write_all(string_return(1, ":1.42"));
    if (!pump_sync_ok(session, 0, "hello pump") ||
        !expect_phase(session, SessionPhase::A11yAddress, "a11y-address after hello")) {
        return false;
    }

    (void)harness.provider.peer(0).write_all(string_return(2, kA11yAddressText));
    if (!pump_sync_ok(session, 0, "address pump") ||
        !expect(harness.provider.connection_count() == 2, "a11y connection opened") ||
        !expect_phase(session, SessionPhase::A11yConnect, "a11y-connect phase")) {
        return false;
    }
    if (!expect_equal(hex(record(read_output(harness.provider.peer(1)))),
                      hex(expected_auth_request()), "a11y SASL request")) {
        return false;
    }

    (void)harness.provider.peer(1).write_all(bytes_of("OK fedcba9876543210\r\n"));
    if (!pump_sync_ok(session, 0, "a11y auth pump") ||
        !expect_phase(session, SessionPhase::A11yHello, "a11y-hello phase")) {
        return false;
    }

    // Drain the a11y BEGIN + Hello so the next read only sees the registry call.
    {
        const ByteStream handshake = record(read_output(harness.provider.peer(1)));
        if (!expect(handshake.size() > 7, "a11y BEGIN plus a message")) {
            return false;
        }
    }

    (void)harness.provider.peer(1).write_all(string_return(3, ":1.7"));
    if (!pump_sync_ok(session, 0, "register pump") ||
        !expect_phase(session, SessionPhase::Registering, "registering phase")) {
        return false;
    }
    {
        const ByteStream bytes = record(read_output(harness.provider.peer(1)));
        const auto messages = split_messages(bytes);
        if (!expect(messages.size() == 1, "one RegisterEvent") ||
            !expect_equal(messages[0].header.member, "RegisterEvent", "RegisterEvent member")) {
            return false;
        }
    }

    (void)harness.provider.peer(1).write_all(empty_return(4));
    if (!pump_sync_ok(session, 0, "ready pump") || !expect(session.ready(), "session is ready")) {
        return false;
    }
    return expect(read_output(harness.provider.peer(1)).empty(), "no bytes after Ready");
}

bool worker_lifecycle() {
    Harness harness;
    if (!drive_worker(harness)) {
        return false;
    }
    const std::size_t completed = harness.session.pumps();
    if (!expect(completed > 0, "the worker completed pumps")) {
        return false;
    }
    harness.session.stop();
    if (!expect(!harness.session.running(), "stop joins the worker") ||
        !expect_phase(harness.session, SessionPhase::Closed, "stopped session is closed") ||
        !expect(harness.session.pumps() >= completed, "pump count is monotonic")) {
        return false;
    }
    // A second stop must be a no-op, and the session must be restartable because the first thread
    // was joined rather than detached.
    harness.session.stop();
    if (!expect(!harness.session.running(), "second stop is a no-op")) {
        return false;
    }
    if (!expect(harness.session.start().has_value(), "restart after stop") ||
        !pump_sync_ok(harness.session, 0, "restart pump") ||
        !expect(harness.provider.connection_count() == 3, "restart opens a fresh connection")) {
        return false;
    }
    harness.session.stop();
    return expect_phase(harness.session, SessionPhase::Closed, "restart stopped cleanly");
}

bool handshake_timeout() {
    SessionConfig config = Harness::make_config();
    config.handshake_timeout_ms = 0;
    Harness harness(config);
    const auto result = harness.session.pump(0);
    if (!expect(!result.has_value(), "a zero budget times out") ||
        !expect(result.error() == SessionError::Timeout, "timeout error is classified") ||
        !expect_phase(harness.session, SessionPhase::Failed, "timeout is terminal") ||
        !expect(harness.session.error() == SessionError::Timeout, "error is recorded")) {
        return false;
    }
    const auto again = harness.session.pump(0);
    return expect(!again.has_value() && again.error() == SessionError::Timeout,
                  "the failure is sticky");
}

bool close_before_ready() {
    Harness harness;
    if (!expect(harness.session.pump(0).has_value(), "connect pump")) {
        return false;
    }
    harness.provider.peer(0).shutdown();
    const auto result = harness.session.pump(0);
    return expect(!result.has_value(), "a close before Ready fails") &&
           expect(result.error() == SessionError::TransportClosed, "close is classified") &&
           expect_phase(harness.session, SessionPhase::Failed, "close is terminal") &&
           expect(harness.session.error() == SessionError::TransportClosed, "close error recorded");
}

bool close_after_ready() {
    Harness harness;
    if (!drive_worker(harness)) {
        return false;
    }
    // The registry disconnects mid-session: the worker's next pump reports a clean close.
    harness.provider.peer(1).shutdown();
    const auto result = harness.session.pump_sync(0);
    const bool reported_closed = result.has_value() && result->closed;
    if (!expect(reported_closed, "the close is reported as a clean shutdown") ||
        !expect_phase(harness.session, SessionPhase::Closed, "session closes")) {
        return false;
    }
    harness.session.stop();
    return expect(!harness.session.running(), "stop after a peer close joins the worker");
}

bool transport_close_during_pump() {
    Harness harness;
    if (!drive_worker(harness)) {
        return false;
    }
    harness.session.stop();
    if (!expect_phase(harness.session, SessionPhase::Closed, "stop closes the session")) {
        return false;
    }
    // With the worker joined, a manual pump reports the closed state instead of touching a dead
    // transport.
    const auto after = harness.session.pump(0);
    return expect(after.has_value() && after->closed, "pump after close reports closed");
}

bool worker_handshake_is_deterministic() {
    Harness first;
    Harness second;
    ByteLog first_steps;
    ByteLog second_steps;
    if (!drive_worker(first, &first_steps) || !drive_worker(second, &second_steps)) {
        return false;
    }
    first.session.stop();
    second.session.stop();
    if (!expect(first_steps.size() == second_steps.size(), "same number of byte chunks")) {
        return false;
    }
    for (std::size_t index = 0; index < first_steps.size(); ++index) {
        if (!expect_equal(hex(first_steps[index]), hex(second_steps[index]),
                          "two worker-driven handshakes emit identical bytes")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!worker_lifecycle()) {
        return 1;
    }
    if (!handshake_timeout()) {
        return 2;
    }
    if (!close_before_ready()) {
        return 3;
    }
    if (!close_after_ready()) {
        return 4;
    }
    if (!transport_close_during_pump()) {
        return 5;
    }
    if (!worker_handshake_is_deterministic()) {
        return 6;
    }
    std::cout << "a11y_atspi_session_pump: ok\n";
    return 0;
}
