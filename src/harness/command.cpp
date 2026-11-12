// SPDX-License-Identifier: MIT

#include "command_internal.hpp"
#include "tessera/harness/command.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::harness {
namespace {

using detail::invalid_argument;
using detail::read_string;
using detail::read_ui_id;
using detail::reject_unknown;

[[nodiscard]] std::expected<CommandPayload, ProtocolError>
decode_snapshot(const JsonValue& arguments) {
    if (auto error = reject_unknown(arguments, {"includeHidden"}); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    SnapshotCommand command;
    if (const JsonValue* value = arguments.find("includeHidden"); value != nullptr) {
        const auto flag = value->as_bool();
        if (!flag.has_value()) {
            return invalid_argument("includeHidden must be a boolean", "includeHidden");
        }
        command.include_hidden = *flag;
    }
    return CommandPayload{command};
}

[[nodiscard]] std::expected<CommandPayload, ProtocolError>
decode_action(ActionKind kind, const JsonValue& arguments) {
    switch (kind) {
    case ActionKind::Click:
    case ActionKind::Focus:
    case ActionKind::ScrollIntoView:
        if (auto error = reject_unknown(arguments, {"uiId", "itemKey"}); error.has_value()) {
            return std::unexpected(std::move(*error));
        }
        break;
    case ActionKind::SetValue:
        if (auto error = reject_unknown(arguments, {"uiId", "itemKey", "value"});
            error.has_value()) {
            return std::unexpected(std::move(*error));
        }
        break;
    case ActionKind::Scroll:
        if (auto error = reject_unknown(arguments, {"uiId", "itemKey", "dx", "dy"});
            error.has_value()) {
            return std::unexpected(std::move(*error));
        }
        break;
    }
    ActionCommand command;
    command.kind = kind;
    const JsonValue* ui_id = arguments.find("uiId");
    if (ui_id == nullptr) {
        return invalid_argument("uiId is required", "uiId");
    }
    if (!read_ui_id(*ui_id, command.ui_id)) {
        return invalid_argument("uiId must be an unsigned decimal string", "uiId");
    }
    if (const JsonValue* item_key = arguments.find("itemKey"); item_key != nullptr) {
        auto text = read_string(item_key);
        if (!text.has_value()) {
            return invalid_argument("itemKey must be a string", "itemKey");
        }
        command.item_key = std::move(*text);
    }
    if (kind == ActionKind::SetValue) {
        const JsonValue* value = arguments.find("value");
        if (value == nullptr) {
            return invalid_argument("value is required for setValue", "value");
        }
        auto text = read_string(value);
        if (!text.has_value()) {
            return invalid_argument("value must be a string", "value");
        }
        command.value = std::move(*text);
    }
    if (kind == ActionKind::Scroll) {
        const JsonValue* dx = arguments.find("dx");
        const JsonValue* dy = arguments.find("dy");
        if (dx == nullptr) {
            return invalid_argument("dx is required for scroll", "dx");
        }
        if (dy == nullptr) {
            return invalid_argument("dy is required for scroll", "dy");
        }
        const auto dx_value = dx->as_number();
        const auto dy_value = dy->as_number();
        if (!dx_value.has_value() || !std::isfinite(*dx_value)) {
            return invalid_argument("dx must be a finite number", "dx");
        }
        if (!dy_value.has_value() || !std::isfinite(*dy_value)) {
            return invalid_argument("dy must be a finite number", "dy");
        }
        command.scroll_dx = *dx_value;
        command.scroll_dy = *dy_value;
    }
    return CommandPayload{std::move(command)};
}

[[nodiscard]] std::expected<CommandPayload, ProtocolError>
decode_trace(const JsonValue& arguments) {
    if (auto error = reject_unknown(arguments, {"path"}); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    const JsonValue* path = arguments.find("path");
    auto text = read_string(path);
    if (path == nullptr) {
        return invalid_argument("path is required for trace", "path");
    }
    if (!text.has_value()) {
        return invalid_argument("path must be a string", "path");
    }
    if (text->empty()) {
        return invalid_argument("path must not be empty", "path");
    }
    TraceCommand command;
    command.path = std::move(*text);
    return CommandPayload{std::move(command)};
}

[[nodiscard]] std::expected<CommandPayload, ProtocolError>
decode_audit(const JsonValue& arguments) {
    if (auto error = reject_unknown(arguments, {"profile", "path"}); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    const JsonValue* profile = arguments.find("profile");
    auto profile_text = read_string(profile);
    if (profile == nullptr) {
        return invalid_argument("profile is required for audit", "profile");
    }
    if (!profile_text.has_value()) {
        return invalid_argument("profile must be a string", "profile");
    }
    if (profile_text->empty()) {
        return invalid_argument("profile must not be empty", "profile");
    }
    AuditCommand command;
    command.profile = std::move(*profile_text);
    if (const JsonValue* path = arguments.find("path"); path != nullptr) {
        auto path_text = read_string(path);
        if (!path_text.has_value()) {
            return invalid_argument("path must be a string", "path");
        }
        command.path = std::move(*path_text);
    }
    return CommandPayload{std::move(command)};
}

[[nodiscard]] std::expected<JsonValue, ProtocolError>
encode_snapshot(const SnapshotCommand& command) {
    JsonValue::Object members;
    members.emplace_back("includeHidden", JsonValue::boolean(command.include_hidden));
    return JsonValue::object(std::move(members));
}

[[nodiscard]] std::expected<JsonValue, ProtocolError> encode_action(const ActionCommand& command) {
    if (!std::isfinite(command.scroll_dx) || !std::isfinite(command.scroll_dy)) {
        return invalid_argument("scroll offsets must be finite", "dx");
    }
    const bool is_set_value = command.kind == ActionKind::SetValue;
    const bool is_scroll = command.kind == ActionKind::Scroll;
    if (!is_set_value && !command.value.empty()) {
        return invalid_argument("value is only valid for setValue", "value");
    }
    if (!is_scroll && (command.scroll_dx != 0.0 || command.scroll_dy != 0.0)) {
        return invalid_argument("dx/dy are only valid for scroll", "dx");
    }
    JsonValue::Object members;
    members.emplace_back("uiId", JsonValue::string(std::to_string(command.ui_id)));
    if (command.item_key.has_value()) {
        members.emplace_back("itemKey", JsonValue::string(*command.item_key));
    }
    if (is_set_value) {
        members.emplace_back("value", JsonValue::string(command.value));
    }
    if (is_scroll) {
        members.emplace_back("dx", JsonValue::number(command.scroll_dx));
        members.emplace_back("dy", JsonValue::number(command.scroll_dy));
    }
    return JsonValue::object(std::move(members));
}

[[nodiscard]] std::expected<JsonValue, ProtocolError> encode_trace(const TraceCommand& command) {
    if (command.path.empty()) {
        return invalid_argument("path must not be empty", "path");
    }
    JsonValue::Object members;
    members.emplace_back("path", JsonValue::string(command.path));
    return JsonValue::object(std::move(members));
}

[[nodiscard]] std::expected<JsonValue, ProtocolError> encode_audit(const AuditCommand& command) {
    if (command.profile.empty()) {
        return invalid_argument("profile must not be empty", "profile");
    }
    JsonValue::Object members;
    members.emplace_back("profile", JsonValue::string(command.profile));
    if (!command.path.empty()) {
        members.emplace_back("path", JsonValue::string(command.path));
    }
    return JsonValue::object(std::move(members));
}

template<typename... Visitors> struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template<typename... Visitors> Overloaded(Visitors...) -> Overloaded<Visitors...>;

} // namespace

std::string_view to_string(ActionKind kind) noexcept {
    switch (kind) {
    case ActionKind::Click:
        return kClickCommand;
    case ActionKind::SetValue:
        return kSetValueCommand;
    case ActionKind::Focus:
        return kFocusCommand;
    case ActionKind::Scroll:
        return kScrollCommand;
    case ActionKind::ScrollIntoView:
        return kScrollIntoViewCommand;
    }
    return kClickCommand;
}

std::optional<ActionKind> action_kind_from_string(std::string_view text) noexcept {
    if (text == kClickCommand) {
        return ActionKind::Click;
    }
    if (text == kSetValueCommand) {
        return ActionKind::SetValue;
    }
    if (text == kFocusCommand) {
        return ActionKind::Focus;
    }
    if (text == kScrollCommand) {
        return ActionKind::Scroll;
    }
    if (text == kScrollIntoViewCommand) {
        return ActionKind::ScrollIntoView;
    }
    return std::nullopt;
}

std::string_view command_name(const CommandPayload& payload) noexcept {
    if (std::holds_alternative<SnapshotCommand>(payload)) {
        return kSnapshotCommand;
    }
    if (const auto* action = std::get_if<ActionCommand>(&payload); action != nullptr) {
        return to_string(action->kind);
    }
    if (std::holds_alternative<TraceCommand>(payload)) {
        return kTraceCommand;
    }
    return kAuditCommand;
}

std::expected<CommandPayload, ProtocolError> decode_command(std::string_view name,
                                                            const JsonValue& arguments) {
    if (name == kSnapshotCommand) {
        return decode_snapshot(arguments);
    }
    if (name == kTraceCommand) {
        return decode_trace(arguments);
    }
    if (name == kAuditCommand) {
        return decode_audit(arguments);
    }
    if (const auto kind = action_kind_from_string(name); kind.has_value()) {
        return decode_action(*kind, arguments);
    }
    return std::unexpected(ProtocolError{ProtocolErrorCode::UnknownCommand, "unknown command",
                                         std::string(name), std::nullopt});
}

std::expected<CommandPayload, ProtocolError> decode_command(const Request& request) {
    return decode_command(request.command, request.arguments);
}

std::expected<JsonValue, ProtocolError> encode_command_arguments(const CommandPayload& command) {
    return std::visit(Overloaded{
                          [](const SnapshotCommand& value) { return encode_snapshot(value); },
                          [](const ActionCommand& value) { return encode_action(value); },
                          [](const TraceCommand& value) { return encode_trace(value); },
                          [](const AuditCommand& value) { return encode_audit(value); },
                      },
                      command);
}

std::expected<std::string, ProtocolError> encode_command(RequestId id,
                                                         const CommandPayload& command) {
    auto arguments = encode_command_arguments(command);
    if (!arguments.has_value()) {
        return std::unexpected(arguments.error());
    }
    Request request;
    request.id = id;
    request.command = std::string(command_name(command));
    request.arguments = std::move(*arguments);
    return encode_request(request);
}

} // namespace tessera::harness
