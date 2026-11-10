// SPDX-License-Identifier: MIT
//
// Shared fixture for the AT-SPI2 bridge dispatch tests: a small snapshot (root, pressable button,
// text field, slider), a method-call encoder, and the reply exchange helper. Split so each test
// executable stays under the file-size limit and covers one part of the interface surface.

#pragma once

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/atspi_bridge.hpp"
#include "tessera/ui/accessibility/dbus_wire.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace a11y_atspi_test {

using tessera::rendering::rhi::Rect;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::AtspiBridge;
using tessera::ui::a11y::MessageHeader;
using tessera::ui::a11y::MessageType;
using tessera::ui::a11y::MessageWriter;
using tessera::ui::a11y::ParsedMessage;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;
using tessera::ui::a11y::WireError;

inline A11yNode make_node(Role role, std::string_view name, std::string_view value, Rect bounds) {
    A11yNode node;
    node.role = role;
    std::copy(name.begin(), name.end(), node.name.begin());
    std::copy(value.begin(), value.end(), node.value.begin());
    node.bounds = bounds;
    return node;
}

// Snapshot layout: 0 root, 1 button "Close" (press+focus), 2 text field "Search" ("hello"),
// 3 slider "Volume" ("40").
inline A11ySnapshot make_snapshot() {
    A11ySnapshot snapshot;
    A11yNode root = make_node(Role::Generic, "Root", "", Rect{0.0f, 0.0f, 200.0f, 100.0f});
    root.id = 1;
    root.child_count = 3;
    root.children[0] = 1;
    root.children[1] = 2;
    root.children[2] = 3;

    A11yNode button = make_node(Role::Button, "Close", "", Rect{10.0f, 10.0f, 20.0f, 20.0f});
    button.id = 2;
    button.states = State::Focusable;
    button.actions = Action::Press | Action::Focus;

    A11yNode field =
        make_node(Role::TextField, "Search", "hello", Rect{40.0f, 10.0f, 60.0f, 20.0f});
    field.id = 3;
    field.states = State::Focusable | State::Focused;
    field.actions = Action::SetValue;

    A11yNode slider = make_node(Role::Slider, "Volume", "40", Rect{110.0f, 10.0f, 80.0f, 20.0f});
    slider.id = 4;
    slider.states = State::Focusable;
    slider.actions = Action::SetValue;

    snapshot.nodes = {root, button, field, slider};
    return snapshot;
}

inline std::string path_of(std::uint16_t index) {
    return "/org/a11y/atspi/accessible/" + std::to_string(index);
}

using Body = std::function<void(MessageWriter&)>;

struct Call {
    std::string path;
    std::string interface_name;
    std::string member;
    std::string signature;
    Body body;
};

inline std::vector<std::uint8_t> encode(const Call& call, std::uint32_t serial = 10) {
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::MethodCall;
    header.serial = serial;
    header.path = call.path;
    header.interface = call.interface_name;
    header.member = call.member;
    header.destination = ":1.42";
    header.sender = ":1.7";
    header.signature = call.signature;
    (void)writer.begin_message(header);
    if (call.body) {
        call.body(writer);
    }
    (void)writer.end_message();
    return std::vector<std::uint8_t>(writer.bytes().begin(), writer.bytes().end());
}

// `bytes` owns the reply, `raw` owns the request; the parsed reply borrows `bytes`, and moving the
// vectors keeps their heap buffers, so the borrow stays valid when the struct is returned.
struct Exchange {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> raw;
    std::expected<ParsedMessage, WireError> reply;
};

inline Exchange exchange(AtspiBridge& bridge, const Call& call) {
    Exchange result;
    result.raw = encode(call);
    (void)bridge.dispatch(result.raw, result.bytes);
    result.reply = tessera::ui::a11y::parse_message(result.bytes);
    return result;
}

inline bool is_return(const Exchange& result, std::string_view signature) {
    return a11y_dbus_test::expect(result.reply.has_value(), "reply parses") &&
           a11y_dbus_test::expect(result.reply->header.type == MessageType::MethodReturn,
                                  "reply is a return") &&
           a11y_dbus_test::expect(result.reply->header.reply_serial == 10, "reply serial") &&
           a11y_dbus_test::expect_equal(result.reply->header.destination, ":1.7",
                                        "reply destination") &&
           a11y_dbus_test::expect_equal(result.reply->header.signature, signature,
                                        "reply signature");
}

inline bool is_error(const Exchange& result, std::string_view name) {
    return a11y_dbus_test::expect(result.reply.has_value(), "error parses") &&
           a11y_dbus_test::expect(result.reply->header.type == MessageType::Error, "error type") &&
           a11y_dbus_test::expect_equal(result.reply->header.error_name, name, "error name");
}

} // namespace a11y_atspi_test
