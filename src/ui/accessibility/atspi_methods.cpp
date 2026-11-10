// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace tessera::ui::a11y {
namespace {

constexpr std::string_view kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

std::int32_t to_int32(float value) {
    return static_cast<std::int32_t>(std::lround(value));
}

} // namespace

bool AtspiBridge::reply_invalid_args(const MessageHeader& call,
                                     std::span<const std::uint8_t> /*body*/,
                                     std::vector<std::uint8_t>& out) {
    start_error(call, kInvalidArgs, "invalid arguments");
    (void)finish_reply(out);
    return true;
}

bool AtspiBridge::handle_accessible(const ParsedMessage& call, A11yIndex index,
                                    std::vector<std::uint8_t>& out) {
    const A11yNode& node = snapshot_.nodes[index];
    const std::string& member = call.header.member;
    const bool is_root = index == 0;

    if (member == "GetName" || member == "GetLocalizedName") {
        start_reply(call.header, "s").write_string(name_of(node));
    } else if (member == "GetDescription") {
        start_reply(call.header, "s").write_string(description_of(node));
    } else if (member == "GetRole") {
        start_reply(call.header, "u")
            .write_uint32(static_cast<std::uint32_t>(atspi_role(node.role)));
    } else if (member == "GetRoleName") {
        start_reply(call.header, "s").write_string(atspi_role_name(node.role));
    } else if (member == "GetState") {
        const StateSet states = atspi_states(node);
        start_reply(call.header, "au").write_array('u', [&states](MessageWriter& array) {
            array.write_uint32(states.low);
            array.write_uint32(states.high);
        });
    } else if (member == "GetInterfaces") {
        const InterfaceSet interfaces = atspi_interfaces(node, is_root);
        std::array<std::string_view, 6> names{};
        const std::size_t count = atspi_interface_names(interfaces, names);
        start_reply(call.header, "as").write_array('s', [&names, count](MessageWriter& array) {
            for (std::size_t position = 0; position < count; ++position) {
                array.write_string(names[position]);
            }
        });
    } else if (member == "GetChildCount") {
        start_reply(call.header, "i").write_int32(node.child_count);
    } else if (member == "GetChildren") {
        MessageWriter& writer = start_reply(call.header, "a(so)");
        writer.write_array('(', [&node, this](MessageWriter& array) {
            for (std::uint16_t child = 0; child < node.child_count; ++child) {
                write_reference(array, node.children[child]);
            }
        });
    } else if (member == "GetChildAtIndex") {
        MessageReader reader(call.body);
        const std::int32_t child = reader.read_int32();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        MessageWriter& writer = start_reply(call.header, "(so)");
        if (child < 0 || child >= static_cast<std::int32_t>(node.child_count)) {
            write_reference(writer, kInvalidIndex);
        } else {
            write_reference(writer, node.children[child]);
        }
    } else if (member == "GetParent") {
        write_reference(start_reply(call.header, "(so)"), parent_of(index));
    } else if (member == "GetIndexInParent") {
        const A11yIndex parent = parent_of(index);
        std::int32_t position = -1;
        if (parent != kInvalidIndex) {
            const A11yNode& parent_node = snapshot_.nodes[parent];
            for (std::uint16_t child = 0; child < parent_node.child_count; ++child) {
                if (parent_node.children[child] == index) {
                    position = child;
                    break;
                }
            }
        }
        start_reply(call.header, "i").write_int32(position);
    } else if (member == "GetApplication") {
        write_reference(start_reply(call.header, "(so)"), 0);
    } else if (member == "GetAttributes") {
        start_reply(call.header, "a{ss}").write_array('{', [](MessageWriter&) {});
    } else if (member == "GetRelationSet") {
        start_reply(call.header, "a(ua(so))").write_array('(', [](MessageWriter&) {});
    } else if (member == "GetAccessibleId") {
        // Automation ids are numeric hashes in this slice; expose nothing rather than a wrong
        // string.
        start_reply(call.header, "s").write_string("");
    } else if (member == "Contains") {
        MessageReader reader(call.body);
        const std::int32_t x = reader.read_int32();
        const std::int32_t y = reader.read_int32();
        (void)reader.read_uint32(); // coordtype: snapshot bounds are already window coordinates
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        const std::int32_t left = to_int32(node.bounds.x);
        const std::int32_t top = to_int32(node.bounds.y);
        const std::int32_t right = left + to_int32(node.bounds.width);
        const std::int32_t bottom = top + to_int32(node.bounds.height);
        start_reply(call.header, "b").write_bool(x >= left && x < right && y >= top && y < bottom);
    } else {
        return false;
    }
    (void)finish_reply(out);
    return true;
}

bool AtspiBridge::handle_application(const ParsedMessage& call, std::vector<std::uint8_t>& out) {
    const std::string& member = call.header.member;
    if (member == "GetLocale") {
        MessageWriter& writer = start_reply(call.header, "(ss)");
        writer.write_string(config_.locale);
        writer.write_string("");
    } else if (member == "GetApplicationBusAddress") {
        start_reply(call.header, "s").write_string(config_.bus_address);
    } else if (member == "GetName" || member == "GetApplicationName") {
        start_reply(call.header, "s").write_string(config_.application_name);
    } else if (member == "GetVersion") {
        start_reply(call.header, "s").write_string(config_.application_version);
    } else if (member == "GetToolkitName") {
        start_reply(call.header, "s").write_string(config_.toolkit_name);
    } else if (member == "GetId") {
        start_reply(call.header, "i").write_int32(0);
    } else {
        return false;
    }
    (void)finish_reply(out);
    return true;
}

bool AtspiBridge::handle_action(const ParsedMessage& call, A11yIndex index,
                                std::vector<std::uint8_t>& out) {
    const A11yNode& node = snapshot_.nodes[index];
    const std::string& member = call.header.member;
    const std::uint32_t count = atspi_action_count(node);

    if (member == "GetActionCount") {
        start_reply(call.header, "i").write_int32(static_cast<std::int32_t>(count));
    } else if (member == "GetActions") {
        MessageWriter& writer = start_reply(call.header, "a(sss)");
        writer.write_array('(', [&node, count](MessageWriter& array) {
            for (std::uint32_t action = 0; action < count; ++action) {
                array.write_struct([&node, action](MessageWriter& entry) {
                    entry.write_string(atspi_action_name(node, action));
                    entry.write_string("");
                    entry.write_string("");
                });
            }
        });
    } else if (member == "GetActionName" || member == "GetActionDescription" ||
               member == "GetKeyBinding") {
        MessageReader reader(call.body);
        const std::int32_t action = reader.read_int32();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        const bool valid = action >= 0 && action < static_cast<std::int32_t>(count);
        MessageWriter& writer = start_reply(call.header, "s");
        writer.write_string(member == "GetActionName" && valid
                                ? atspi_action_name(node, static_cast<std::uint32_t>(action))
                                : std::string_view{});
    } else if (member == "DoAction") {
        MessageReader reader(call.body);
        const std::int32_t action = reader.read_int32();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        const bool valid = action >= 0 && action < static_cast<std::int32_t>(count);
        // The bridge never mutates UI state: it queues a command for the UI thread (section 10.1).
        const bool accepted =
            valid && queue_action(ActionRequest{index, Action::Press,
                                                static_cast<std::uint32_t>(action), 0.0});
        start_reply(call.header, "b").write_bool(accepted);
    } else {
        return false;
    }
    (void)finish_reply(out);
    return true;
}

bool AtspiBridge::handle_component(const ParsedMessage& call, A11yIndex index,
                                   std::vector<std::uint8_t>& out) {
    const A11yNode& node = snapshot_.nodes[index];
    const std::string& member = call.header.member;
    const std::int32_t left = to_int32(node.bounds.x);
    const std::int32_t top = to_int32(node.bounds.y);
    const std::int32_t width = to_int32(node.bounds.width);
    const std::int32_t height = to_int32(node.bounds.height);

    if (member == "GetExtents") {
        (void)MessageReader(call.body).read_uint32(); // coordtype
        MessageWriter& writer = start_reply(call.header, "(iiii)");
        writer.write_int32(left);
        writer.write_int32(top);
        writer.write_int32(width);
        writer.write_int32(height);
    } else if (member == "GetPosition") {
        (void)MessageReader(call.body).read_uint32();
        MessageWriter& writer = start_reply(call.header, "(ii)");
        writer.write_int32(left);
        writer.write_int32(top);
    } else if (member == "GetSize") {
        MessageWriter& writer = start_reply(call.header, "(ii)");
        writer.write_int32(width);
        writer.write_int32(height);
    } else if (member == "GetLayer") {
        start_reply(call.header, "u").write_uint32(static_cast<std::uint32_t>(atspi_layer(node)));
    } else if (member == "GetMDIZOrder") {
        start_reply(call.header, "i").write_int32(0);
    } else if (member == "GetAlpha") {
        start_reply(call.header, "d").write_double(1.0);
    } else if (member == "GrabFocus") {
        const bool accepted = queue_action(ActionRequest{index, Action::Focus, 0, 0.0});
        start_reply(call.header, "b").write_bool(accepted);
    } else if (member == "Contains") {
        MessageReader reader(call.body);
        const std::int32_t x = reader.read_int32();
        const std::int32_t y = reader.read_int32();
        (void)reader.read_uint32();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        start_reply(call.header, "b")
            .write_bool(x >= left && x < left + width && y >= top && y < top + height);
    } else {
        return false;
    }
    (void)finish_reply(out);
    return true;
}

bool AtspiBridge::handle_text(const ParsedMessage& call, A11yIndex index,
                              std::vector<std::uint8_t>& out) {
    const A11yNode& node = snapshot_.nodes[index];
    const std::string& member = call.header.member;
    const std::string_view text = atspi_text(node);

    if (member == "GetCharacterCount") {
        // Byte counts in this slice: the snapshot stores UTF-8 with no run/grapheme model yet.
        start_reply(call.header, "i").write_int32(static_cast<std::int32_t>(text.size()));
    } else if (member == "GetText") {
        MessageReader reader(call.body);
        std::int32_t start = reader.read_int32();
        std::int32_t end = reader.read_int32();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        const auto size = static_cast<std::int32_t>(text.size());
        start = std::clamp(start, 0, size);
        end = std::clamp(end, start, size);
        start_reply(call.header, "s")
            .write_string(text.substr(static_cast<std::size_t>(start),
                                      static_cast<std::size_t>(end - start)));
    } else if (member == "GetTextAtOffset") {
        MessageReader reader(call.body);
        (void)reader.read_int32();  // offset
        (void)reader.read_uint32(); // boundary
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        // One run for the whole text: the snapshot has no line/word segmentation yet.
        MessageWriter& writer = start_reply(call.header, "(sii)");
        writer.write_string(text);
        writer.write_int32(0);
        writer.write_int32(static_cast<std::int32_t>(text.size()));
    } else if (member == "GetCaretOffset") {
        start_reply(call.header, "i").write_int32(0);
    } else if (member == "SetCaretOffset") {
        start_reply(call.header, "b").write_bool(false);
    } else if (member == "GetSelectionCount") {
        start_reply(call.header, "i").write_int32(0);
    } else if (member == "GetSelection") {
        start_reply(call.header, "a(ii)").write_array('(', [](MessageWriter&) {});
    } else {
        return false;
    }
    (void)finish_reply(out);
    return true;
}

bool AtspiBridge::handle_value(const ParsedMessage& call, A11yIndex index,
                               std::vector<std::uint8_t>& out) {
    const A11yNode& node = snapshot_.nodes[index];
    const std::string& member = call.header.member;
    const AtspiValue value = atspi_value(node);

    if (member == "GetCurrentValue") {
        start_reply(call.header, "d").write_double(value.current);
    } else if (member == "GetMinimumValue") {
        start_reply(call.header, "d").write_double(value.minimum);
    } else if (member == "GetMaximumValue") {
        start_reply(call.header, "d").write_double(value.maximum);
    } else if (member == "GetMinimumIncrement") {
        start_reply(call.header, "d").write_double(value.increment);
    } else if (member == "SetCurrentValue") {
        MessageReader reader(call.body);
        const double requested = reader.read_double();
        if (reader.failed()) {
            return reply_invalid_args(call.header, call.body, out);
        }
        const bool accepted =
            value.has_value && queue_action(ActionRequest{index, Action::SetValue, 0, requested});
        start_reply(call.header, "b").write_bool(accepted);
    } else {
        return false;
    }
    (void)finish_reply(out);
    return true;
}

} // namespace tessera::ui::a11y
