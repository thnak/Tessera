// SPDX-License-Identifier: MIT
//
// `AtspiSession` handshake replay: the exact bytes the session must send for the session-bus
// Hello, the `org.a11y.Bus.GetAddress` request, the accessibility-bus Hello, and the
// `Registry.RegisterEvent` mask, each replayed against the daemon replies it expects. The session
// is pumped directly on the test thread, so the byte order is fully deterministic.

#include "a11y_session_fixture.hpp"
#include "tessera/ui/accessibility/atspi_events.hpp"
#include "tessera/ui/accessibility/atspi_session.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using namespace a11y_session_test;
using tessera::ui::a11y::kA11yBusMember;
using tessera::ui::a11y::kA11yBusName;
using tessera::ui::a11y::kA11yBusPath;
using tessera::ui::a11y::kEventRegistrations;
using tessera::ui::a11y::kRegistryBusName;
using tessera::ui::a11y::kRegistryInterface;
using tessera::ui::a11y::kRegistryMember;
using tessera::ui::a11y::kRegistryPath;
using tessera::ui::a11y::kSessionBusInterface;
using tessera::ui::a11y::kSessionBusMember;
using tessera::ui::a11y::kSessionBusName;
using tessera::ui::a11y::kSessionBusPath;

using ByteStream = std::vector<std::uint8_t>;
using ByteLog = std::vector<ByteStream>;

std::vector<std::uint8_t> hello_call(std::uint32_t serial) {
    return encode_call(serial, kSessionBusName, kSessionBusPath, kSessionBusInterface,
                       kSessionBusMember, std::string_view{}, [](MessageWriter&) {});
}

std::vector<std::uint8_t> get_address_call(std::uint32_t serial) {
    return encode_call(serial, kA11yBusName, kA11yBusPath, kA11yBusName, kA11yBusMember,
                       std::string_view{}, [](MessageWriter&) {});
}

std::vector<std::uint8_t> register_event_call(std::uint32_t serial) {
    return encode_call(serial, kRegistryBusName, kRegistryPath, kRegistryInterface, kRegistryMember,
                       "as", [](MessageWriter& writer) {
                           writer.write_array('s', [](MessageWriter& array) {
                               for (const std::string_view name : kEventRegistrations) {
                                   array.write_string(name);
                               }
                           });
                       });
}

bool expect_phase(const AtspiSession& session, SessionPhase phase, const char* message) {
    return expect(session.phase() == phase, message);
}

// Drives the whole handshake from a fresh session, asserting each chunk of bytes before replaying
// the daemon's answer. When `log` is non-null every chunk is also appended for determinism checks.
bool drive_handshake(Harness& harness, ByteLog* log = nullptr) {
    auto record = [log](const ByteStream& bytes) {
        if (log != nullptr) {
            log->push_back(bytes);
        }
        return bytes;
    };
    AtspiSession& session = harness.session;
    const std::string auth = expected_auth_request();

    // 1. The first pump opens the session bus and sends SASL EXTERNAL.
    if (!pump_ok(session, 0, "connect pump")) {
        return false;
    }
    if (!expect_equal(hex(record(read_output(harness.provider.peer(0)))), hex(auth),
                      "SASL EXTERNAL request bytes")) {
        return false;
    }
    if (!expect_phase(session, SessionPhase::SessionConnect, "session-connect phase")) {
        return false;
    }

    // 2. OK -> BEGIN plus the session-bus Hello (serial 1).
    (void)harness.provider.peer(0).write_all(bytes_of("OK 0123456789abcdef\r\n"));
    if (!pump_ok(session, 0, "auth pump")) {
        return false;
    }
    const ByteStream begin = bytes_of("BEGIN\r\n");
    const ByteStream hello = hello_call(1);
    const ByteStream second = record(read_output(harness.provider.peer(0)));
    if (!expect_equal(hex(second), hex(concat(begin, hello)), "BEGIN + Hello bytes")) {
        return false;
    }
    const auto hello_messages = split_messages(std::span(second).subspan(begin.size()));
    if (!expect(hello_messages.size() == 1, "one Hello message")) {
        return false;
    }
    if (!expect_message(hello_messages[0], MessageType::MethodCall, kSessionBusPath,
                        kSessionBusInterface, kSessionBusMember, kSessionBusName, "")) {
        return false;
    }
    if (!expect(hello_messages[0].header.serial == 1, "Hello carries serial 1") ||
        !expect_phase(session, SessionPhase::SessionHello, "session-hello phase")) {
        return false;
    }

    // 3. Hello reply -> `org.a11y.Bus.GetAddress` (serial 2).
    (void)harness.provider.peer(0).write_all(string_return(1, ":1.42"));
    if (!pump_ok(session, 0, "hello pump")) {
        return false;
    }
    const ByteStream third = record(read_output(harness.provider.peer(0)));
    if (!expect_equal(hex(third), hex(get_address_call(2)), "GetAddress bytes") ||
        !expect_equal(session.unique_name(), ":1.42", "session unique name") ||
        !expect_equal(harness.bridge.bus_name(), ":1.42", "bridge bus name") ||
        !expect_phase(session, SessionPhase::A11yAddress, "a11y-address phase")) {
        return false;
    }

    // 4. Address reply -> connect/auth the accessibility bus and release the session bus.
    (void)harness.provider.peer(0).write_all(string_return(2, kA11yAddressText));
    if (!pump_ok(session, 0, "address pump")) {
        return false;
    }
    // A peer write after the switch fails with Closed: the session released its session-bus end.
    const auto stale = harness.provider.peer(0).write_all(bytes_of("stale"));
    if (!expect(harness.provider.connection_count() == 2, "a second connection was opened") ||
        !expect(!stale.has_value(), "session bus connection closed")) {
        return false;
    }
    if (!expect_equal(hex(record(read_output(harness.provider.peer(1)))), hex(auth),
                      "a11y SASL request bytes")) {
        return false;
    }
    if (!expect_equal(session.a11y_bus_address(), kA11yAddressText, "a11y bus address") ||
        !expect_equal(harness.bridge.config().bus_address, kA11yAddressText,
                      "bridge reports the a11y bus address") ||
        !expect_phase(session, SessionPhase::A11yConnect, "a11y-connect phase")) {
        return false;
    }

    // 5. a11y OK -> BEGIN plus the accessibility-bus Hello (serial 3).
    (void)harness.provider.peer(1).write_all(bytes_of("OK fedcba9876543210\r\n"));
    if (!pump_ok(session, 0, "a11y auth pump")) {
        return false;
    }
    const ByteStream fifth = record(read_output(harness.provider.peer(1)));
    if (!expect_equal(hex(fifth), hex(concat(begin, hello_call(3))), "a11y BEGIN + Hello") ||
        !expect_phase(session, SessionPhase::A11yHello, "a11y-hello phase")) {
        return false;
    }

    // 6. Hello reply -> `Registry.RegisterEvent` with the three object event names (serial 4).
    (void)harness.provider.peer(1).write_all(string_return(3, ":1.7"));
    if (!pump_ok(session, 0, "register pump")) {
        return false;
    }
    const ByteStream sixth = record(read_output(harness.provider.peer(1)));
    if (!expect_equal(hex(sixth), hex(register_event_call(4)), "RegisterEvent bytes") ||
        !expect_phase(session, SessionPhase::Registering, "registering phase")) {
        return false;
    }
    // Decode the payload independently of the encoder used for the comparison.
    const auto registered = split_messages(sixth);
    if (!expect(registered.size() == 1, "one RegisterEvent message")) {
        return false;
    }
    MessageReader reader(registered[0].body);
    if (!expect(reader.read_array_header('s').has_value(), "RegisterEvent body is an array")) {
        return false;
    }
    std::vector<std::string> names;
    while (!reader.array_done()) {
        names.emplace_back(reader.read_string());
    }
    if (!expect(reader.finish_array().has_value(), "event array is well framed") ||
        !expect(names.size() == kEventRegistrations.size(), "three event names registered") ||
        !expect_equal(names[0], "object:state-changed", "state-changed registered") ||
        !expect_equal(names[1], "object:property-change", "property-change registered") ||
        !expect_equal(names[2], "object:bounds-changed", "bounds-changed registered")) {
        return false;
    }

    // 7. The registry's empty reply completes the handshake.
    (void)harness.provider.peer(1).write_all(empty_return(4));
    if (!pump_ok(session, 0, "ready pump")) {
        return false;
    }
    return expect(session.ready(), "session is ready") &&
           expect_equal(session.unique_name(), ":1.7", "a11y unique name exposed") &&
           expect(read_output(harness.provider.peer(1)).empty(), "no extra bytes after Ready");
}

bool handshake_replay() {
    Harness harness;
    return drive_handshake(harness);
}

bool handshake_is_deterministic() {
    Harness first;
    Harness second;
    ByteLog first_steps;
    ByteLog second_steps;
    if (!drive_handshake(first, &first_steps) || !drive_handshake(second, &second_steps)) {
        return false;
    }
    if (!expect(first_steps.size() == second_steps.size(), "same number of byte chunks")) {
        return false;
    }
    for (std::size_t index = 0; index < first_steps.size(); ++index) {
        if (!expect_equal(hex(first_steps[index]), hex(second_steps[index]),
                          "identical replayed bytes")) {
            return false;
        }
    }
    return true;
}

// Publishes one root node so the dispatcher has something to answer.
void publish_root(Harness& harness) {
    tessera::ui::a11y::A11ySnapshot snapshot;
    tessera::ui::a11y::A11yNode node;
    node.id = 1;
    node.role = tessera::ui::a11y::Role::Generic;
    set_node_text(node.name, "root");
    snapshot.nodes.push_back(node);
    harness.bridge.publish(snapshot);
}

bool dispatch_after_ready() {
    Harness harness;
    if (!drive_handshake(harness)) {
        return false;
    }
    publish_root(harness);

    // An AT calls Accessible.GetName on the root object.
    const auto call =
        encode_call(77, ":1.7", "/org/a11y/atspi/accessible/0", "org.a11y.atspi.Accessible",
                    "GetName", std::string_view{}, [](MessageWriter&) {});
    (void)harness.provider.peer(1).write_all(call);
    if (!pump_ok(harness.session, 0, "dispatch pump")) {
        return false;
    }
    const ByteStream reply = read_output(harness.provider.peer(1));
    const auto messages = split_messages(reply);
    if (!expect(messages.size() == 1, "one reply")) {
        return false;
    }
    MessageReader body(messages[0].body);
    const std::string_view name = body.read_string();
    if (!expect(messages[0].header.type == MessageType::MethodReturn, "method return") ||
        !expect(messages[0].header.reply_serial == 77, "reply correlates by serial") ||
        !expect_equal(name, "root", "GetName body")) {
        return false;
    }

    // The Application interface now reports the accessibility bus address.
    const auto address_call =
        encode_call(78, ":1.7", "/org/a11y/atspi/accessible/0", "org.a11y.atspi.Application",
                    "GetApplicationBusAddress", std::string_view{}, [](MessageWriter&) {});
    (void)harness.provider.peer(1).write_all(address_call);
    if (!pump_ok(harness.session, 0, "address dispatch pump")) {
        return false;
    }
    const ByteStream address_reply = read_output(harness.provider.peer(1));
    const auto address_messages = split_messages(address_reply);
    if (!expect(address_messages.size() == 1, "one address reply")) {
        return false;
    }
    MessageReader address_body(address_messages[0].body);
    return expect_equal(address_body.read_string(), kA11yAddressText,
                        "GetApplicationBusAddress reports the a11y bus");
}

bool emits_events_when_ready() {
    Harness harness;
    if (!drive_handshake(harness)) {
        return false;
    }
    publish_root(harness);
    if (!expect(harness.bridge.post_event(tessera::ui::a11y::make_state_event(0, "focused", true)),
                "event queued")) {
        return false;
    }
    if (!pump_ok(harness.session, 0, "event pump")) {
        return false;
    }
    const ByteStream signal_bytes = read_output(harness.provider.peer(1));
    const auto messages = split_messages(signal_bytes);
    if (!expect(messages.size() == 1, "one signal")) {
        return false;
    }
    return expect(messages[0].header.type == MessageType::Signal, "signal type") &&
           expect_equal(messages[0].header.interface, "org.a11y.atspi.Event.Object",
                        "event interface") &&
           expect_equal(messages[0].header.member, "StateChanged", "state-changed member") &&
           expect_equal(messages[0].header.path, "/org/a11y/atspi/accessible/0",
                        "event object path");
}

} // namespace

int main() {
    if (!handshake_replay()) {
        return 1;
    }
    if (!handshake_is_deterministic()) {
        return 2;
    }
    if (!dispatch_after_ready()) {
        return 3;
    }
    if (!emits_events_when_ready()) {
        return 4;
    }
    std::cout << "a11y_atspi_session: ok\n";
    return 0;
}
