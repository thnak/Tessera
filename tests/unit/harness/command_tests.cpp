// SPDX-License-Identifier: MIT

#include "tessera/harness/command.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

using tessera::harness::action_kind_from_string;
using tessera::harness::ActionCommand;
using tessera::harness::ActionKind;
using tessera::harness::AuditCommand;
using tessera::harness::command_name;
using tessera::harness::CommandPayload;
using tessera::harness::decode_command;
using tessera::harness::decode_request;
using tessera::harness::encode_command;
using tessera::harness::ProtocolError;
using tessera::harness::ProtocolErrorCode;
using tessera::harness::SnapshotCommand;
using tessera::harness::to_string;
using tessera::harness::TraceCommand;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_command: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string request_text(std::uint64_t id, const CommandPayload& command) {
    const auto text = encode_command(id, command);
    return text.has_value() ? *text : std::string("<encode failed>");
}

std::optional<ProtocolErrorCode> decode_flat(std::string_view name, std::string_view parameters) {
    std::string line = "{\"id\":1,\"cmd\":\"";
    line += name;
    line += '"';
    if (!parameters.empty()) {
        line += ',';
        line += parameters;
    }
    line += '}';
    const auto request = decode_request(line);
    if (!request.has_value()) {
        return request.error().code;
    }
    const auto command = decode_command(*request);
    if (command.has_value()) {
        return std::nullopt;
    }
    return command.error().code;
}

template<typename T> const T* payload(const CommandPayload& command) {
    return std::get_if<T>(&command);
}

void check_decoding() {
    // The spec's examples decode straight into the typed model.
    const auto snapshot = decode_flat("snapshot", "");
    expect(!snapshot.has_value(), "snapshot decodes");
    const auto request = decode_request(R"({"id":2,"cmd":"snapshot","includeHidden":true})");
    const auto decoded = decode_command(*request);
    expect(decoded.has_value(), "snapshot with includeHidden decodes");
    expect(payload<SnapshotCommand>(*decoded) != nullptr, "snapshot payload kind");
    expect(payload<SnapshotCommand>(*decoded)->include_hidden, "includeHidden honoured");

    const auto click = decode_request(R"({"id":3,"cmd":"click","uiId":"222222222222222222"})");
    const auto click_command = decode_command(*click);
    expect(click_command.has_value(), "click decodes");
    const ActionCommand* action = payload<ActionCommand>(*click_command);
    expect(action != nullptr && action->kind == ActionKind::Click, "click kind");
    expect(action->ui_id == 222222222222222222ULL, "click uiId");
    expect(!action->item_key.has_value(), "no item key by default");

    // List items are addressed by (uiId, itemKey).
    const auto item = decode_request(R"({"id":4,"cmd":"focus","uiId":"7","itemKey":"row-3"})");
    const auto item_command = decode_command(*item);
    expect(payload<ActionCommand>(*item_command)->item_key == std::optional<std::string>{"row-3"},
           "item key decodes");

    const auto set_value =
        decode_request(R"({"id":5,"cmd":"setValue","uiId":"8","value":"alice"})");
    const auto set_value_command = decode_command(*set_value);
    expect(payload<ActionCommand>(*set_value_command)->value == "alice", "setValue text");

    // Clearing an input is a legitimate empty value.
    const auto clear = decode_request(R"({"id":6,"cmd":"setValue","uiId":"8","value":""})");
    const auto clear_command = decode_command(*clear);
    expect(payload<ActionCommand>(*clear_command)->value.empty(), "empty setValue");

    const auto scroll = decode_request(R"({"id":7,"cmd":"scroll","uiId":"9","dx":-16,"dy":120.5})");
    const auto scroll_command = decode_command(*scroll);
    const ActionCommand* scrolled = payload<ActionCommand>(*scroll_command);
    expect(scrolled != nullptr && scrolled->kind == ActionKind::Scroll, "scroll kind");
    expect(scrolled->scroll_dx == -16.0 && scrolled->scroll_dy == 120.5, "scroll offsets");

    const auto trace = decode_request(R"({"id":8,"cmd":"trace","path":"out/trace.json"})");
    const auto trace_command = decode_command(*trace);
    expect(payload<TraceCommand>(*trace_command)->path == "out/trace.json", "trace path");

    const auto audit = decode_request(R"({"id":9,"cmd":"audit","profile":"desktop"})");
    const auto audit_command = decode_command(*audit);
    const AuditCommand* audited = payload<AuditCommand>(*audit_command);
    expect(audited != nullptr && audited->profile == "desktop" && audited->path.empty(),
           "audit profile with default path");

    // The uint64 maximum survives because UiIds are decimal strings on the wire.
    const auto maximum = decode_request(R"({"id":10,"cmd":"click","uiId":"18446744073709551615"})");
    const auto maximum_command = decode_command(*maximum);
    expect(payload<ActionCommand>(*maximum_command)->ui_id ==
               std::numeric_limits<std::uint64_t>::max(),
           "uint64 max uiId");
}

void check_rejections() {
    expect(decode_flat("teleport", "") == ProtocolErrorCode::UnknownCommand, "unknown command");
    expect(decode_flat("snapshot", R"("include_hidden":true)") ==
               ProtocolErrorCode::InvalidArgument,
           "unknown field rejected");
    expect(decode_flat("click", "") == ProtocolErrorCode::InvalidArgument, "missing uiId");
    expect(decode_flat("click", R"("uiId":"abc")") == ProtocolErrorCode::InvalidArgument,
           "non-numeric uiId");
    expect(decode_flat("click", R"("uiId":"01")") == ProtocolErrorCode::InvalidArgument,
           "non-canonical uiId");
    expect(decode_flat("click", R"("uiId":-1)") == ProtocolErrorCode::InvalidArgument,
           "negative uiId");
    expect(decode_flat("click", R"("uiId":1.5)") == ProtocolErrorCode::InvalidArgument,
           "fractional uiId");
    expect(decode_flat("click", R"("uiId":1,"value":"x")") == ProtocolErrorCode::InvalidArgument,
           "click has no value field");
    expect(decode_flat("setValue", R"("uiId":1)") == ProtocolErrorCode::InvalidArgument,
           "setValue requires value");
    expect(decode_flat("setValue", R"("uiId":1,"value":5)") == ProtocolErrorCode::InvalidArgument,
           "setValue value must be a string");
    expect(decode_flat("scrollIntoView", R"("uiId":1,"dx":1)") ==
               ProtocolErrorCode::InvalidArgument,
           "scrollIntoView takes no offsets");
    expect(decode_flat("scroll", R"("uiId":1,"dx":1)") == ProtocolErrorCode::InvalidArgument,
           "scroll requires dy");
    expect(decode_flat("scroll", R"("uiId":1,"dx":"1","dy":2)") ==
               ProtocolErrorCode::InvalidArgument,
           "scroll offset must be numeric");
    expect(decode_flat("focus", R"("uiId":1,"itemKey":5)") == ProtocolErrorCode::InvalidArgument,
           "itemKey must be a string");
    expect(decode_flat("snapshot", R"("includeHidden":"yes")") ==
               ProtocolErrorCode::InvalidArgument,
           "includeHidden must be a boolean");
    expect(decode_flat("trace", "") == ProtocolErrorCode::InvalidArgument, "trace requires path");
    expect(decode_flat("trace", R"("path":"")") == ProtocolErrorCode::InvalidArgument,
           "trace path must not be empty");
    expect(decode_flat("audit", "") == ProtocolErrorCode::InvalidArgument,
           "audit requires a profile");
    expect(decode_flat("audit", R"("profile":"")") == ProtocolErrorCode::InvalidArgument,
           "audit profile must not be empty");
    expect(decode_flat("audit", R"("profile":"desktop","path":5)") ==
               ProtocolErrorCode::InvalidArgument,
           "audit path must be a string");

    // Decoding a request whose arguments are not an object is a command error, not a crash.
    const auto request = decode_request(R"({"id":1,"cmd":"snapshot","includeHidden":true})");
    expect(decode_command(*request).has_value(), "request arguments decode");
}

void check_encoding() {
    SnapshotCommand snapshot;
    expect(request_text(2, snapshot) ==
               R"({"protocolVersion":1,"id":"2","cmd":"snapshot","includeHidden":false})",
           "snapshot request bytes");
    snapshot.include_hidden = true;
    expect(request_text(2, snapshot) ==
               R"({"protocolVersion":1,"id":"2","cmd":"snapshot","includeHidden":true})",
           "includeHidden request bytes");

    ActionCommand click;
    click.kind = ActionKind::Click;
    click.ui_id = 222222222222222222ULL;
    expect(request_text(3, click) ==
               R"({"protocolVersion":1,"id":"3","cmd":"click","uiId":"222222222222222222"})",
           "click request bytes");

    ActionCommand set_value;
    set_value.kind = ActionKind::SetValue;
    set_value.ui_id = 8;
    set_value.value = "alice";
    expect(request_text(4, set_value) ==
               R"({"protocolVersion":1,"id":"4","cmd":"setValue","uiId":"8","value":"alice"})",
           "setValue request bytes");

    ActionCommand scroll;
    scroll.kind = ActionKind::Scroll;
    scroll.ui_id = 9;
    scroll.item_key = "row-3";
    scroll.scroll_dx = -16.0;
    scroll.scroll_dy = 120.5;
    expect(
        request_text(5, scroll) ==
            R"({"protocolVersion":1,"id":"5","cmd":"scroll","uiId":"9","itemKey":"row-3","dx":-16,"dy":120.5})",
        "scroll request bytes");

    AuditCommand audit;
    audit.profile = "desktop";
    audit.path = "out/audit.json";
    expect(
        request_text(6, audit) ==
            R"({"protocolVersion":1,"id":"6","cmd":"audit","profile":"desktop","path":"out/audit.json"})",
        "audit request bytes");

    // Round-trips hold for every kind, including the action sub-kinds.
    const CommandPayload payloads[] = {
        SnapshotCommand{true}, click, set_value, scroll, audit, TraceCommand{"out/trace.json"}};
    for (const CommandPayload& command : payloads) {
        const auto text = encode_command(11, command);
        if (!expect(text.has_value(), "round-trip encode")) {
            continue;
        }
        const auto request = decode_request(*text);
        if (!expect(request.has_value(), "round-trip decode")) {
            continue;
        }
        const auto decoded = decode_command(*request);
        expect(decoded.has_value() && *decoded == command, "command round-trip");
    }

    // Encoding rejects a payload whose unused fields are set: the bytes must mean exactly what the
    // typed command says.
    ActionCommand bogus_click;
    bogus_click.kind = ActionKind::Click;
    bogus_click.ui_id = 1;
    bogus_click.value = "ignored?";
    expect(encode_command(1, bogus_click).error().code == ProtocolErrorCode::InvalidArgument,
           "click with value cannot be encoded");
    bogus_click.value.clear();
    bogus_click.scroll_dy = 1.0;
    expect(encode_command(1, bogus_click).error().code == ProtocolErrorCode::InvalidArgument,
           "click with scroll offsets cannot be encoded");
    expect(encode_command(1, TraceCommand{}).error().code == ProtocolErrorCode::InvalidArgument,
           "empty trace path cannot be encoded");
    expect(encode_command(1, AuditCommand{}).error().code == ProtocolErrorCode::InvalidArgument,
           "empty audit profile cannot be encoded");
}

void check_names() {
    expect(to_string(ActionKind::Click) == "click", "click name");
    expect(to_string(ActionKind::SetValue) == "setValue", "setValue name");
    expect(to_string(ActionKind::Focus) == "focus", "focus name");
    expect(to_string(ActionKind::Scroll) == "scroll", "scroll name");
    expect(to_string(ActionKind::ScrollIntoView) == "scrollIntoView", "scrollIntoView name");
    for (const ActionKind kind : {ActionKind::Click, ActionKind::SetValue, ActionKind::Focus,
                                  ActionKind::Scroll, ActionKind::ScrollIntoView}) {
        expect(action_kind_from_string(to_string(kind)) == std::optional<ActionKind>{kind},
               "action kind round-trip");
    }
    expect(!action_kind_from_string("Snapshot").has_value(), "action names are case-sensitive");

    expect(command_name(SnapshotCommand{}) == "snapshot", "snapshot command name");
    expect(command_name(ActionCommand{}) == "click", "default action name");
    expect(command_name(TraceCommand{}) == "trace", "trace command name");
    expect(command_name(AuditCommand{}) == "audit", "audit command name");
}

} // namespace

int main() {
    check_decoding();
    check_rejections();
    check_encoding();
    check_names();
    if (failures != 0) {
        std::cerr << "harness_command: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
