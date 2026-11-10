// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_bridge.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace tessera::ui::a11y {
namespace {

constexpr std::string_view kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

// Property names per interface, in the order `GetAll` emits them. AT-SPI2 clients read properties
// instead of calling the equivalent getters when they can, so the set covers what a screen reader
// touches while walking the tree.
constexpr std::array<std::string_view, 6> kAccessibleProperties{
    "Name", "Description", "Parent", "ChildCount", "Interfaces", "AccessibleId"};
constexpr std::array<std::string_view, 6> kApplicationProperties{
    "Name", "ToolkitName", "Version", "AtspiVersion", "Id", "Locale"};
constexpr std::array<std::string_view, 2> kTextProperties{"CharacterCount", "CaretOffset"};
constexpr std::array<std::string_view, 4> kValueProperties{"CurrentValue", "MinimumValue",
                                                           "MaximumValue", "MinimumIncrement"};
constexpr std::array<std::string_view, 0> kNoProperties{};

std::span<const std::string_view> property_names(std::string_view interface) {
    if (interface == kAccessibleInterface) {
        return kAccessibleProperties;
    }
    if (interface == kApplicationInterface) {
        return kApplicationProperties;
    }
    if (interface == kTextInterface) {
        return kTextProperties;
    }
    if (interface == kValueInterface) {
        return kValueProperties;
    }
    return kNoProperties;
}

bool known_interface(std::string_view interface) {
    return interface == kAccessibleInterface || interface == kApplicationInterface ||
           interface == kComponentInterface || interface == kActionInterface ||
           interface == kTextInterface || interface == kValueInterface;
}

} // namespace

bool AtspiBridge::write_property_value(MessageWriter& writer, A11yIndex index,
                                       std::string_view interface, std::string_view property) {
    const A11yNode& node = snapshot_.nodes[index];
    if (interface == kAccessibleInterface) {
        if (property == "Name") {
            writer.write_variant(
                "s", [&node](MessageWriter& value) { value.write_string(name_of(node)); });
        } else if (property == "Description") {
            writer.write_variant(
                "s", [&node](MessageWriter& value) { value.write_string(description_of(node)); });
        } else if (property == "Parent") {
            writer.write_variant("(so)", [this, index](MessageWriter& value) {
                write_reference(value, parent_of(index));
            });
        } else if (property == "ChildCount") {
            writer.write_variant(
                "i", [&node](MessageWriter& value) { value.write_int32(node.child_count); });
        } else if (property == "Interfaces") {
            const InterfaceSet interfaces = atspi_interfaces(node, index == 0);
            std::array<std::string_view, 6> names{};
            const std::size_t count = atspi_interface_names(interfaces, names);
            writer.write_variant("as", [&names, count](MessageWriter& value) {
                value.write_array('s', [&names, count](MessageWriter& array) {
                    for (std::size_t position = 0; position < count; ++position) {
                        array.write_string(names[position]);
                    }
                });
            });
        } else if (property == "AccessibleId") {
            // Automation ids are numeric hashes in this slice; expose nothing rather than a wrong
            // string (same rule as `GetAccessibleId`).
            writer.write_variant("s", [](MessageWriter& value) { value.write_string(""); });
        } else {
            return false;
        }
        return true;
    }
    if (interface == kApplicationInterface) {
        if (index != 0) {
            return false;
        }
        if (property == "Name") {
            writer.write_variant("s", [this](MessageWriter& value) {
                value.write_string(config_.application_name);
            });
        } else if (property == "ToolkitName") {
            writer.write_variant(
                "s", [this](MessageWriter& value) { value.write_string(config_.toolkit_name); });
        } else if (property == "Version") {
            writer.write_variant("s", [this](MessageWriter& value) {
                value.write_string(config_.application_version);
            });
        } else if (property == "AtspiVersion") {
            writer.write_variant("s", [](MessageWriter& value) { value.write_string("2.1"); });
        } else if (property == "Id") {
            writer.write_variant("i", [](MessageWriter& value) { value.write_int32(0); });
        } else if (property == "Locale") {
            writer.write_variant(
                "s", [this](MessageWriter& value) { value.write_string(config_.locale); });
        } else {
            return false;
        }
        return true;
    }
    if (interface == kTextInterface) {
        if (!atspi_interfaces(node, index == 0).text) {
            return false;
        }
        if (property == "CharacterCount") {
            writer.write_variant("i", [&node](MessageWriter& value) {
                value.write_int32(static_cast<std::int32_t>(atspi_text(node).size()));
            });
        } else if (property == "CaretOffset") {
            writer.write_variant("i", [](MessageWriter& value) { value.write_int32(0); });
        } else {
            return false;
        }
        return true;
    }
    if (interface == kValueInterface) {
        if (!atspi_interfaces(node, index == 0).value) {
            return false;
        }
        const AtspiValue value = atspi_value(node);
        if (property == "CurrentValue") {
            writer.write_variant(
                "d", [&value](MessageWriter& target) { target.write_double(value.current); });
        } else if (property == "MinimumValue") {
            writer.write_variant(
                "d", [&value](MessageWriter& target) { target.write_double(value.minimum); });
        } else if (property == "MaximumValue") {
            writer.write_variant(
                "d", [&value](MessageWriter& target) { target.write_double(value.maximum); });
        } else if (property == "MinimumIncrement") {
            writer.write_variant(
                "d", [&value](MessageWriter& target) { target.write_double(value.increment); });
        } else {
            return false;
        }
        return true;
    }
    return false;
}

bool AtspiBridge::handle_properties(const ParsedMessage& call, A11yIndex index,
                                    std::vector<std::uint8_t>& out) {
    const std::string& member = call.header.member;
    if (member == "Get") {
        MessageReader reader(call.body);
        const std::string_view interface = reader.read_string();
        const std::string_view property = reader.read_string();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        MessageWriter& writer = start_reply(call.header, "v");
        if (!write_property_value(writer, index, interface, property)) {
            start_error(call.header, kInvalidArgs, "unknown property");
        }
        (void)finish_reply(out);
        return true;
    }
    if (member == "GetAll") {
        MessageReader reader(call.body);
        const std::string_view interface = reader.read_string();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        if (!known_interface(interface)) {
            start_error(call.header, kInvalidArgs, "unknown interface");
            (void)finish_reply(out);
            return true;
        }
        MessageWriter& writer = start_reply(call.header, "a{sv}");
        writer.write_array('{', [this, &interface, index](MessageWriter& array) {
            for (const std::string_view property : property_names(interface)) {
                array.begin_struct();
                array.write_string(property);
                if (!write_property_value(array, index, interface, property)) {
                    // A property the node's interfaces do not implement is simply absent; an
                    // unwritten struct would produce a malformed array, so write a null string.
                    array.write_variant("s", [](MessageWriter& value) { value.write_string(""); });
                }
                array.end_struct();
            }
        });
        (void)finish_reply(out);
        return true;
    }
    return false;
}

} // namespace tessera::ui::a11y
