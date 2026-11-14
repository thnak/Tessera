// SPDX-License-Identifier: MIT
//
// AT-SPI2 interface dispatch: Accessible, Application, Action, Component, Text, and Value method
// calls answered from the published semantics snapshot, plus the action-request queue.

#include "a11y_atspi_fixture.hpp"

#include <array>
#include <string>
#include <vector>

namespace {

using namespace a11y_atspi_test;
using a11y_dbus_test::expect;
using a11y_dbus_test::expect_equal;
using tessera::ui::a11y::ActionRequest;
using tessera::ui::a11y::AtspiState;
using tessera::ui::a11y::has_state;
using tessera::ui::a11y::kAccessibleInterface;
using tessera::ui::a11y::kActionInterface;
using tessera::ui::a11y::kApplicationInterface;
using tessera::ui::a11y::kComponentInterface;
using tessera::ui::a11y::kTextInterface;
using tessera::ui::a11y::kValueInterface;
using tessera::ui::a11y::MessageReader;

bool name_role_and_state(AtspiBridge& bridge) {
    {
        const Exchange result = exchange(
            bridge, Call{path_of(1), std::string(kAccessibleInterface), "GetName", "", {}});
        if (!is_return(result, "s")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect_equal(reader.read_string(), "Close", "GetName body")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(1), std::string(kAccessibleInterface), "GetRole", "", {}});
        if (!is_return(result, "u")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_uint32() == 42, "GetRole is push button")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(2), std::string(kAccessibleInterface), "GetRoleName", "", {}});
        if (!is_return(result, "s")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect_equal(reader.read_string(), "entry", "GetRoleName")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(2), std::string(kAccessibleInterface), "GetState", "", {}});
        if (!is_return(result, "au")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_array_header('u').has_value(), "state array")) {
            return false;
        }
        const std::uint32_t low = reader.read_uint32();
        const std::uint32_t high = reader.read_uint32();
        if (!expect(reader.finish_array().has_value(), "state array ends")) {
            return false;
        }
        const tessera::ui::a11y::StateSet states{low, high};
        if (!expect(has_state(states, AtspiState::Focused), "focused state bit") ||
            !expect(has_state(states, AtspiState::Editable), "editable state bit")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(0), std::string(kAccessibleInterface), "GetInterfaces", "", {}});
        if (!is_return(result, "as")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_array_header('s').has_value(), "interface array")) {
            return false;
        }
        std::vector<std::string> names;
        while (!reader.array_done()) {
            names.emplace_back(reader.read_string());
        }
        if (!expect(reader.finish_array().has_value(), "interface array ends")) {
            return false;
        }
        return expect(std::find(names.begin(), names.end(), kAccessibleInterface) != names.end(),
                      "accessible interface listed") &&
               expect(std::find(names.begin(), names.end(), kApplicationInterface) != names.end(),
                      "root lists application") &&
               expect(std::find(names.begin(), names.end(), kComponentInterface) != names.end(),
                      "component interface listed");
    }
}

bool hierarchy(AtspiBridge& bridge) {
    {
        const Exchange result = exchange(
            bridge, Call{path_of(0), std::string(kAccessibleInterface), "GetChildren", "", {}});
        if (!is_return(result, "a(so)")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_array_header('(').has_value(), "children array")) {
            return false;
        }
        std::vector<std::string> paths;
        while (!reader.array_done()) {
            (void)reader.begin_struct();
            const std::string bus = std::string(reader.read_string());
            const std::string path = std::string(reader.read_object_path());
            reader.end_struct();
            if (!expect_equal(bus, ":1.42", "child bus name")) {
                return false;
            }
            paths.push_back(path);
        }
        if (!expect(reader.finish_array().has_value(), "children array ends") ||
            !expect(paths.size() == 3, "three children") ||
            !expect_equal(paths[1], path_of(2), "second child path")) {
            return false;
        }
    }
    {
        const Exchange result =
            exchange(bridge, Call{path_of(1), std::string(kAccessibleInterface), "GetChildAtIndex",
                                  "i", [](MessageWriter& writer) { writer.write_int32(9); }});
        if (!is_return(result, "(so)")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        (void)reader.begin_struct();
        const std::string bus = std::string(reader.read_string());
        if (!expect(bus.empty(), "out of range child is the null reference")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(2), std::string(kAccessibleInterface), "GetParent", "", {}});
        if (!is_return(result, "(so)")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        (void)reader.begin_struct();
        (void)reader.read_string();
        if (!expect_equal(reader.read_object_path(), path_of(0), "parent is the root")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge,
            Call{path_of(2), std::string(kAccessibleInterface), "GetIndexInParent", "", {}});
        if (!is_return(result, "i")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_int32() == 1, "index in parent")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(1), std::string(kAccessibleInterface), "GetApplication", "", {}});
        if (!is_return(result, "(so)")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        (void)reader.begin_struct();
        (void)reader.read_string();
        return expect_equal(reader.read_object_path(), path_of(0), "application is the root");
    }
}

bool text_and_value(AtspiBridge& bridge) {
    {
        const Exchange result = exchange(bridge, Call{path_of(2), std::string(kTextInterface),
                                                      "GetText", "ii", [](MessageWriter& writer) {
                                                          writer.write_int32(1);
                                                          writer.write_int32(4);
                                                      }});
        if (!is_return(result, "s")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect_equal(reader.read_string(), "ell", "GetText substring")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(2), std::string(kTextInterface), "GetCharacterCount", "", {}});
        if (!is_return(result, "i")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_int32() == 5, "character count")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(bridge, Call{path_of(2), std::string(kTextInterface),
                                                      "GetText", "ii", [](MessageWriter& writer) {
                                                          writer.write_int32(-3);
                                                          writer.write_int32(99);
                                                      }});
        if (!is_return(result, "s")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect_equal(reader.read_string(), "hello", "clamped GetText")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(3), std::string(kValueInterface), "GetCurrentValue", "", {}});
        if (!is_return(result, "d")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_double() == 40.0, "slider current value")) {
            return false;
        }
    }
    {
        const Exchange result =
            exchange(bridge, Call{path_of(3), std::string(kValueInterface), "SetCurrentValue", "d",
                                  [](MessageWriter& writer) { writer.write_double(55.5); }});
        if (!is_return(result, "b")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_bool(), "set value is accepted")) {
            return false;
        }
        std::array<ActionRequest, 4> actions{};
        const std::size_t taken = bridge.take_actions(actions);
        return expect(taken == 1, "one queued action") &&
               expect(actions[0].node == 3 && actions[0].action == Action::SetValue,
                      "set value request") &&
               expect(actions[0].value == 55.5, "set value payload");
    }
}

bool action_requests(AtspiBridge& bridge) {
    {
        const Exchange result = exchange(
            bridge, Call{path_of(1), std::string(kActionInterface), "GetActionCount", "", {}});
        if (!is_return(result, "i")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_int32() == 1, "button action count")) {
            return false;
        }
    }
    {
        const Exchange result =
            exchange(bridge, Call{path_of(1), std::string(kActionInterface), "DoAction", "i",
                                  [](MessageWriter& writer) { writer.write_int32(0); }});
        if (!is_return(result, "b")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_bool(), "DoAction accepted")) {
            return false;
        }
    }
    {
        // A text field has no AT-SPI2 action: the call is refused without a request.
        const Exchange result =
            exchange(bridge, Call{path_of(2), std::string(kActionInterface), "DoAction", "i",
                                  [](MessageWriter& writer) { writer.write_int32(0); }});
        if (!is_return(result, "b")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(!reader.read_bool(), "DoAction refused for a text field")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(2), std::string(kComponentInterface), "GrabFocus", "", {}});
        if (!is_return(result, "b")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        if (!expect(reader.read_bool(), "GrabFocus accepted")) {
            return false;
        }
    }
    std::array<ActionRequest, 8> actions{};
    const std::size_t taken = bridge.take_actions(actions);
    return expect(taken == 2, "press and focus are queued once each") &&
           expect(actions[0].node == 1 && actions[0].action == Action::Press, "press request") &&
           expect(actions[1].node == 2 && actions[1].action == Action::Focus, "focus request") &&
           expect(bridge.take_actions(actions) == 0, "queue drains");
}

bool component(AtspiBridge& bridge) {
    {
        const Exchange result =
            exchange(bridge, Call{path_of(1), std::string(kComponentInterface), "GetExtents", "u",
                                  [](MessageWriter& writer) { writer.write_uint32(1); }});
        if (!is_return(result, "(iiii)")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        const std::int32_t x = reader.read_int32();
        const std::int32_t y = reader.read_int32();
        const std::int32_t width = reader.read_int32();
        const std::int32_t height = reader.read_int32();
        if (!expect(x == 10 && y == 10 && width == 20 && height == 20, "extents")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(bridge, Call{path_of(1), std::string(kComponentInterface),
                                                      "Contains", "iiu", [](MessageWriter& writer) {
                                                          writer.write_int32(15);
                                                          writer.write_int32(15);
                                                          writer.write_uint32(1);
                                                      }});
        if (!is_return(result, "b")) {
            return false;
        }
        MessageReader inside(result.reply->body);
        if (!expect(inside.read_bool(), "point inside")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(bridge, Call{path_of(1), std::string(kComponentInterface),
                                                      "Contains", "iiu", [](MessageWriter& writer) {
                                                          writer.write_int32(200);
                                                          writer.write_int32(15);
                                                          writer.write_uint32(1);
                                                      }});
        if (!is_return(result, "b")) {
            return false;
        }
        MessageReader outside(result.reply->body);
        if (!expect(!outside.read_bool(), "point outside")) {
            return false;
        }
    }
    {
        const Exchange result = exchange(
            bridge, Call{path_of(1), std::string(kComponentInterface), "GetLayer", "", {}});
        if (!is_return(result, "u")) {
            return false;
        }
        MessageReader reader(result.reply->body);
        return expect(reader.read_uint32() == 3, "button is on the widget layer");
    }
}

bool application(AtspiBridge& bridge) {
    const Exchange result =
        exchange(bridge, Call{path_of(0), std::string(kApplicationInterface), "GetLocale", "", {}});
    if (!is_return(result, "(ss)")) {
        return false;
    }
    MessageReader reader(result.reply->body);
    return expect_equal(reader.read_string(), "en", "application locale");
}

} // namespace

int main() {
    tessera::ui::a11y::BridgeConfig config;
    config.bus_address = "unix:path=/tmp/a11y";
    AtspiBridge bridge{config};
    bridge.publish(make_snapshot());
    bridge.set_bus_name(":1.42");

    if (!name_role_and_state(bridge)) {
        return 1;
    }
    if (!hierarchy(bridge)) {
        return 2;
    }
    if (!text_and_value(bridge)) {
        return 3;
    }
    if (!action_requests(bridge)) {
        return 4;
    }
    if (!component(bridge)) {
        return 5;
    }
    if (!application(bridge)) {
        return 6;
    }
    std::cout << "a11y_atspi_bridge: ok\n";
    return 0;
}
