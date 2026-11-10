// SPDX-License-Identifier: MIT
//
// `org.freedesktop.DBus.Properties` `Get`/`GetAll`: the fixed property table the AT-SPI2 bridge
// serves for the interfaces it exposes, the variant signatures it must produce, and the error
// values for unknown interfaces, properties, and malformed bodies.

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/atspi_bridge.hpp"
#include "tessera/ui/accessibility/atspi_events.hpp"
#include "tessera/ui/accessibility/atspi_mapping.hpp"
#include "tessera/ui/accessibility/dbus_wire.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::AtspiBridge;
using tessera::ui::a11y::kMaxTextBytes;
using tessera::ui::a11y::MessageHeader;
using tessera::ui::a11y::MessageReader;
using tessera::ui::a11y::MessageType;
using tessera::ui::a11y::MessageWriter;
using tessera::ui::a11y::parse_message;
using tessera::ui::a11y::ParsedMessage;
using tessera::ui::a11y::Role;

// Copies text into a bounded snapshot field.
void set_node_text(std::array<char, kMaxTextBytes>& target, std::string_view text) {
    const std::size_t count = text.size() < target.size() ? text.size() : target.size() - 1;
    std::copy_n(text.begin(), count, target.begin());
    target[count] = '\0';
}

constexpr std::string_view kProperties = "org.freedesktop.DBus.Properties";
constexpr std::string_view kPath = "/org/a11y/atspi/accessible/0";
constexpr std::string_view kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
constexpr std::string_view kUnknownMethod = "org.freedesktop.DBus.Error.UnknownMethod";

std::vector<std::uint8_t> encode_call(std::string_view member, std::string_view signature,
                                      const std::vector<std::string>& strings,
                                      std::uint32_t serial = 10) {
    MessageWriter writer;
    MessageHeader header;
    header.type = MessageType::MethodCall;
    header.serial = serial;
    header.destination = ":1.7";
    header.path = std::string(kPath);
    header.interface = std::string(kProperties);
    header.member = std::string(member);
    header.signature = std::string(signature);
    (void)writer.begin_message(header);
    for (const std::string& text : strings) {
        writer.write_string(text);
    }
    (void)writer.end_message();
    return std::vector<std::uint8_t>(writer.bytes().begin(), writer.bytes().end());
}

// A slider has Value and Text, plus the always-present Accessible/Component interfaces.
A11ySnapshot make_snapshot() {
    A11ySnapshot snapshot;
    A11yNode node;
    node.id = 1;
    node.role = Role::Slider;
    set_node_text(node.name, "volume");
    set_node_text(node.description, "output level");
    set_node_text(node.value, "40");
    snapshot.nodes.push_back(node);
    return snapshot;
}

std::vector<std::uint8_t> dispatch(const std::vector<std::uint8_t>& call) {
    AtspiBridge bridge;
    bridge.publish(make_snapshot());
    std::vector<std::uint8_t> out;
    (void)bridge.dispatch(call, out);
    return out;
}

std::vector<ParsedMessage> messages_of(const std::vector<std::uint8_t>& bytes) {
    std::vector<ParsedMessage> messages;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto parsed = parse_message(std::span<const std::uint8_t>(bytes).subspan(offset));
        if (!parsed) {
            break;
        }
        messages.push_back(*parsed);
        offset += parsed->consumed;
    }
    return messages;
}

bool get_property_is_a_variant() {
    const auto bytes = dispatch(encode_call(
        "Get", "ss", {std::string(tessera::ui::a11y::kAccessibleInterface), std::string("Name")}));
    const auto messages = messages_of(bytes);
    if (!expect(messages.size() == 1, "one Get reply")) {
        return false;
    }
    const ParsedMessage& reply = messages[0];
    MessageReader reader(reply.body);
    const auto signature = reader.read_variant_signature();
    if (!expect(reply.header.type == MessageType::MethodReturn, "Get returns") ||
        !expect(reply.header.reply_serial == 10, "Get correlates by serial") ||
        !expect_equal(reply.header.signature, "v", "Get reply signature") ||
        !expect(signature.has_value() && *signature == "s", "Name is a string variant")) {
        return false;
    }
    return expect_equal(reader.read_string(), "volume", "Accessible.Name value");
}

bool get_numeric_and_structured_properties() {
    const auto count_bytes = dispatch(encode_call(
        "Get", "ss",
        {std::string(tessera::ui::a11y::kAccessibleInterface), std::string("ChildCount")}));
    const auto count_messages = messages_of(count_bytes);
    if (!expect(count_messages.size() == 1, "ChildCount replied")) {
        return false;
    }
    MessageReader count_reader(count_messages[0].body);
    (void)count_reader.read_variant_signature();
    if (!expect(count_reader.read_int32() == 0, "ChildCount is zero")) {
        return false;
    }

    const auto parent_bytes = dispatch(
        encode_call("Get", "ss",
                    {std::string(tessera::ui::a11y::kAccessibleInterface), std::string("Parent")}));
    const auto parent_messages = messages_of(parent_bytes);
    if (!expect(parent_messages.size() == 1, "Parent replied")) {
        return false;
    }
    MessageReader parent_reader(parent_messages[0].body);
    const auto signature = parent_reader.read_variant_signature();
    if (!expect(signature.has_value() && *signature == "(so)", "Parent is a reference")) {
        return false;
    }
    if (!expect(parent_reader.begin_struct().has_value(), "Parent struct")) {
        return false;
    }
    const std::string_view bus = parent_reader.read_string();
    const std::string_view path = parent_reader.read_object_path();
    parent_reader.end_struct();
    return expect(bus.empty(), "the root has no parent bus name") &&
           expect_equal(path, tessera::ui::a11y::kNullObjectPath, "null parent path");
}

bool value_properties() {
    const auto bytes = dispatch(encode_call(
        "Get", "ss",
        {std::string(tessera::ui::a11y::kValueInterface), std::string("CurrentValue")}));
    const auto messages = messages_of(bytes);
    if (!expect(messages.size() == 1, "CurrentValue replied")) {
        return false;
    }
    MessageReader reader(messages[0].body);
    const auto signature = reader.read_variant_signature();
    return expect(signature.has_value() && *signature == "d", "CurrentValue is a double") &&
           expect(std::abs(reader.read_double() - 40.0) < 1e-9,
                  "CurrentValue follows the snapshot");
}

bool unknown_property_and_interface() {
    const auto bad_bytes = dispatch(encode_call(
        "Get", "ss", {std::string(tessera::ui::a11y::kAccessibleInterface), std::string("Nope")}));
    const auto bad_property = messages_of(bad_bytes);
    if (!expect(bad_property.size() == 1, "unknown property replied")) {
        return false;
    }
    if (!expect(bad_property[0].header.type == MessageType::Error,
                "unknown property is an error") ||
        !expect_equal(bad_property[0].header.error_name, kInvalidArgs, "InvalidArgs error name")) {
        return false;
    }
    const auto interface_bytes =
        dispatch(encode_call("Get", "ss", {std::string("org.example.Bogus"), std::string("Name")}));
    const auto bad_interface = messages_of(interface_bytes);
    return expect(bad_interface.size() == 1 && bad_interface[0].header.type == MessageType::Error,
                  "unknown interface is an error");
}

bool get_all_lists_the_table() {
    const auto bytes = dispatch(
        encode_call("GetAll", "s", {std::string(tessera::ui::a11y::kAccessibleInterface)}));
    const auto messages = messages_of(bytes);
    if (!expect(messages.size() == 1, "one GetAll reply") ||
        !expect_equal(messages[0].header.signature, "a{sv}", "GetAll signature")) {
        return false;
    }
    MessageReader reader(messages[0].body);
    if (!expect(reader.read_array_header('{').has_value(), "GetAll body is a dict array")) {
        return false;
    }
    std::vector<std::string> keys;
    std::string name_value;
    while (!reader.array_done()) {
        if (!expect(reader.begin_struct().has_value(), "dict entry struct")) {
            return false;
        }
        keys.emplace_back(reader.read_string());
        const auto signature = reader.read_variant_signature();
        if (!expect(signature.has_value(), "dict entry variant")) {
            return false;
        }
        if (*signature == "s") {
            const std::string_view text = reader.read_string();
            if (keys.back() == "Name") {
                name_value = std::string(text);
            }
        } else if (*signature == "i") {
            (void)reader.read_int32();
        } else if (*signature == "as") {
            if (!expect(reader.read_array_header('s').has_value(), "interfaces array")) {
                return false;
            }
            while (!reader.array_done()) {
                (void)reader.read_string();
            }
            if (!expect(reader.finish_array().has_value(), "interfaces array framed")) {
                return false;
            }
        } else {
            (void)reader.skip_value(*signature);
        }
        reader.end_struct();
    }
    if (!expect(reader.finish_array().has_value(), "dict array framed") ||
        !expect(keys.size() == 6, "six Accessible properties")) {
        return false;
    }
    return expect(keys[0] == "Name" && keys[1] == "Description" && keys[2] == "Parent" &&
                      keys[3] == "ChildCount" && keys[4] == "Interfaces" &&
                      keys[5] == "AccessibleId",
                  "property order is stable") &&
           expect_equal(name_value, "volume", "GetAll carries the name");
}

bool unknown_interface_get_all() {
    const auto bytes = dispatch(encode_call("GetAll", "s", {"org.example.Bogus"}));
    const auto messages = messages_of(bytes);
    return expect(messages.size() == 1, "GetAll error replied") &&
           expect(messages[0].header.type == MessageType::Error, "GetAll unknown is an error") &&
           expect_equal(messages[0].header.error_name, kInvalidArgs, "InvalidArgs for GetAll");
}

bool malformed_and_unknown_members() {
    // `Get` with one string is not a valid argument list.
    const auto malformed_bytes = dispatch(encode_call("Get", "s", {"org.a11y.atspi.Accessible"}));
    const auto malformed = messages_of(malformed_bytes);
    if (!expect(malformed.size() == 1 && malformed[0].header.type == MessageType::Error,
                "malformed Get is an error")) {
        return false;
    }
    const auto set_bytes = dispatch(encode_call(
        "Set", "ss", {std::string(tessera::ui::a11y::kAccessibleInterface), std::string("Name")}));
    const auto unknown = messages_of(set_bytes);
    return expect(unknown.size() == 1 && unknown[0].header.type == MessageType::Error,
                  "Set is not served") &&
           expect_equal(unknown[0].header.error_name, kUnknownMethod, "UnknownMethod for Set");
}

} // namespace

int main() {
    if (!get_property_is_a_variant()) {
        return 1;
    }
    if (!get_numeric_and_structured_properties()) {
        return 2;
    }
    if (!value_properties()) {
        return 3;
    }
    if (!unknown_property_and_interface()) {
        return 4;
    }
    if (!get_all_lists_the_table()) {
        return 5;
    }
    if (!unknown_interface_get_all()) {
        return 6;
    }
    if (!malformed_and_unknown_members()) {
        return 7;
    }
    std::cout << "a11y_atspi_properties: ok\n";
    return 0;
}
