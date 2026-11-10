// SPDX-License-Identifier: MIT
//
// AT-SPI2 bridge robustness and event delivery: error replies for unknown objects/methods and bad
// arguments, snapshot replacement, and the bounded coalescing event queue turned into D-Bus
// signals.

#include "a11y_atspi_fixture.hpp"

#include <string>
#include <vector>

namespace {

using namespace a11y_atspi_test;
using a11y_dbus_test::expect;
using a11y_dbus_test::expect_equal;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::kAccessibleInterface;
using tessera::ui::a11y::kComponentInterface;
using tessera::ui::a11y::make_focus_event;
using tessera::ui::a11y::MessageReader;

bool error_replies(AtspiBridge& bridge) {
    const Exchange unknown_method =
        exchange(bridge, Call{path_of(1), std::string(kAccessibleInterface), "NotAMethod", "", {}});
    if (!is_error(unknown_method, "org.freedesktop.DBus.Error.UnknownMethod")) {
        return false;
    }
    const Exchange unknown_object = exchange(
        bridge,
        Call{"/org/example/accessible/1", std::string(kAccessibleInterface), "GetName", "", {}});
    if (!is_error(unknown_object, "org.freedesktop.DBus.Error.UnknownObject")) {
        return false;
    }
    const Exchange unknown_interface =
        exchange(bridge, Call{path_of(1), "org.example.NotATspi", "GetName", "", {}});
    if (!is_error(unknown_interface, "org.freedesktop.DBus.Error.UnknownMethod")) {
        return false;
    }
    // A short argument list is InvalidArgs, not a crash or a partially written body.
    const Exchange bad_args =
        exchange(bridge, Call{path_of(1), std::string(kComponentInterface), "Contains", "", {}});
    return expect(bad_args.reply.has_value(), "bad-args reply parses") &&
           expect(bad_args.reply->header.type == tessera::ui::a11y::MessageType::Error,
                  "bad args is an error") &&
           expect_equal(bad_args.reply->header.error_name, "org.freedesktop.DBus.Error.InvalidArgs",
                        "bad args error name");
}

bool publishes_replace_state(AtspiBridge& bridge) {
    bridge.publish(A11ySnapshot{});
    const Exchange result =
        exchange(bridge, Call{path_of(1), std::string(kAccessibleInterface), "GetName", "", {}});
    return is_error(result, "org.freedesktop.DBus.Error.UnknownObject") &&
           expect(bridge.node_count() == 0, "published snapshot replaced");
}

bool event_signals(AtspiBridge& bridge) {
    bridge.publish(make_snapshot());
    bridge.set_bus_name(":1.42");
    if (!expect(bridge.post_event(make_focus_event(1)), "focus event queued") ||
        !expect(bridge.pending_events() == 1, "one pending event")) {
        return false;
    }
    std::vector<std::uint8_t> out;
    if (!expect(bridge.emit_events(out) == 1, "one event emitted")) {
        return false;
    }
    const auto parsed = tessera::ui::a11y::parse_message(out);
    if (!expect(parsed.has_value(), "signal parses") ||
        !expect(parsed->header.type == tessera::ui::a11y::MessageType::Signal,
                "event is a signal") ||
        !expect_equal(parsed->header.interface, "org.a11y.atspi.Event.Object", "event interface") ||
        !expect_equal(parsed->header.member, "StateChanged", "event member") ||
        !expect_equal(parsed->header.path, path_of(1), "event path") ||
        !expect_equal(parsed->header.signature, "siiva{sv}", "event signature")) {
        return false;
    }
    MessageReader reader(parsed->body);
    const std::string_view detail = reader.read_string();
    const std::int32_t detail1 = reader.read_int32();
    const std::int32_t detail2 = reader.read_int32();
    const auto value_signature = reader.read_variant_signature();
    const std::string_view value =
        value_signature && *value_signature == "s" ? reader.read_string() : std::string_view{};
    if (!expect_equal(detail, "focused", "event detail name") ||
        !expect(detail1 == 1 && detail2 == 0, "event details") ||
        !expect(value.empty(), "focus event has no value text")) {
        return false;
    }
    if (!expect(reader.read_array_header('{').has_value(), "event properties array") ||
        !expect(reader.finish_array().has_value(), "event properties array ends")) {
        return false;
    }
    // A stale node index (past the published snapshot) is consumed but emits no bytes.
    std::vector<std::uint8_t> stale;
    (void)bridge.post_event(make_focus_event(200));
    return expect(bridge.emit_events(stale) == 1, "stale event consumed") &&
           expect(stale.empty(), "stale event emits no bytes") &&
           expect(bridge.pending_events() == 0, "queue drains");
}

bool coalesced_events(AtspiBridge& bridge) {
    bridge.publish(make_snapshot());
    (void)bridge.post_event(make_focus_event(1));
    (void)bridge.post_event(make_focus_event(1));
    const std::size_t before = bridge.coalesced_events();
    std::vector<std::uint8_t> out;
    return expect(bridge.emit_events(out) == 1, "coalesced focus emits once") &&
           expect(before == 1, "coalescing counted") &&
           expect(bridge.dropped_events() == 0, "nothing dropped");
}

bool empty_batch(AtspiBridge& bridge) {
    bridge.publish(make_snapshot());
    std::vector<std::uint8_t> out;
    return expect(bridge.emit_events(out) == 0, "no events to emit") &&
           expect(out.empty(), "no bytes");
}

} // namespace

int main() {
    tessera::ui::a11y::BridgeConfig config;
    config.bus_address = "unix:path=/tmp/a11y";
    AtspiBridge bridge{config};
    bridge.publish(make_snapshot());
    bridge.set_bus_name(":1.42");

    if (!error_replies(bridge)) {
        return 1;
    }
    if (!coalesced_events(bridge)) {
        return 2;
    }
    if (!event_signals(bridge)) {
        return 3;
    }
    if (!empty_batch(bridge)) {
        return 4;
    }
    if (!publishes_replace_state(bridge)) {
        return 5;
    }
    std::cout << "a11y_atspi_bridge_events: ok\n";
    return 0;
}
