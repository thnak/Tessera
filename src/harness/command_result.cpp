// SPDX-License-Identifier: MIT

#include "command_internal.hpp"
#include "tessera/harness/command_result.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
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

template<typename... Visitors> struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template<typename... Visitors> Overloaded(Visitors...) -> Overloaded<Visitors...>;

[[nodiscard]] std::expected<JsonValue, ProtocolError>
encode_snapshot_result(const SnapshotResult& result) {
    if (!result.snapshot.is_object()) {
        return invalid_argument("snapshot result must be a JSON object", "snapshot");
    }
    JsonValue::Object members;
    members.emplace_back("snapshot", result.snapshot);
    members.emplace_back("includeHidden", JsonValue::boolean(result.include_hidden));
    return JsonValue::object(std::move(members));
}

[[nodiscard]] std::expected<JsonValue, ProtocolError>
encode_action_result(const ActionResult& result) {
    JsonValue::Object members;
    members.emplace_back("action", JsonValue::string(std::string(to_string(result.kind))));
    members.emplace_back("uiId", JsonValue::string(std::to_string(result.ui_id)));
    if (result.item_key.has_value()) {
        members.emplace_back("itemKey", JsonValue::string(*result.item_key));
    }
    return JsonValue::object(std::move(members));
}

[[nodiscard]] std::expected<JsonValue, ProtocolError>
encode_trace_result(const TraceResult& result) {
    if (result.path.empty()) {
        return invalid_argument("trace path must not be empty", "path");
    }
    if (result.span_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return invalid_argument("spanCount is out of range", "spanCount");
    }
    JsonValue::Object members;
    members.emplace_back("path", JsonValue::string(result.path));
    members.emplace_back("spanCount",
                         JsonValue::integer(static_cast<std::int64_t>(result.span_count)));
    return JsonValue::object(std::move(members));
}

[[nodiscard]] std::expected<JsonValue, ProtocolError>
encode_audit_result(const AuditResult& result) {
    if (result.profile.empty()) {
        return invalid_argument("audit profile must not be empty", "profile");
    }
    if (!std::isfinite(result.score)) {
        return invalid_argument("audit score must be finite", "score");
    }
    JsonValue::Object members;
    members.emplace_back("profile", JsonValue::string(result.profile));
    if (!result.path.empty()) {
        members.emplace_back("path", JsonValue::string(result.path));
    }
    members.emplace_back("score", JsonValue::number(result.score));
    members.emplace_back("passed", JsonValue::boolean(result.passed));
    return JsonValue::object(std::move(members));
}

[[nodiscard]] std::expected<CommandResult, ProtocolError>
decode_snapshot_result(const JsonValue& value) {
    if (auto error = reject_unknown(value, {"snapshot", "includeHidden"}); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    const JsonValue* snapshot = value.find("snapshot");
    if (snapshot == nullptr || !snapshot->is_object()) {
        return invalid_argument("snapshot result must carry a snapshot object", "snapshot");
    }
    SnapshotResult result;
    result.snapshot = *snapshot;
    if (const JsonValue* include_hidden = value.find("includeHidden"); include_hidden != nullptr) {
        const auto flag = include_hidden->as_bool();
        if (!flag.has_value()) {
            return invalid_argument("includeHidden must be a boolean", "includeHidden");
        }
        result.include_hidden = *flag;
    }
    return CommandResult{std::move(result)};
}

[[nodiscard]] std::expected<CommandResult, ProtocolError>
decode_action_result(const JsonValue& value) {
    if (auto error = reject_unknown(value, {"action", "uiId", "itemKey"}); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    const JsonValue* action = value.find("action");
    const auto action_text = read_string(action);
    if (action == nullptr) {
        return invalid_argument("action result must carry action", "action");
    }
    if (!action_text.has_value()) {
        return invalid_argument("action must be a string", "action");
    }
    const auto kind = action_kind_from_string(*action_text);
    if (!kind.has_value()) {
        return invalid_argument("action must be a known action kind", "action");
    }
    ActionResult result;
    result.kind = *kind;
    const JsonValue* ui_id = value.find("uiId");
    if (ui_id == nullptr) {
        return invalid_argument("action result must carry uiId", "uiId");
    }
    if (!read_ui_id(*ui_id, result.ui_id)) {
        return invalid_argument("uiId must be an unsigned decimal string", "uiId");
    }
    if (const JsonValue* item_key = value.find("itemKey"); item_key != nullptr) {
        auto text = read_string(item_key);
        if (!text.has_value()) {
            return invalid_argument("itemKey must be a string", "itemKey");
        }
        result.item_key = std::move(*text);
    }
    return CommandResult{std::move(result)};
}

[[nodiscard]] std::expected<CommandResult, ProtocolError>
decode_trace_result(const JsonValue& value) {
    if (auto error = reject_unknown(value, {"path", "spanCount"}); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    const JsonValue* path = value.find("path");
    auto path_text = read_string(path);
    if (path == nullptr) {
        return invalid_argument("trace result must carry path", "path");
    }
    if (!path_text.has_value()) {
        return invalid_argument("path must be a string", "path");
    }
    if (path_text->empty()) {
        return invalid_argument("trace path must not be empty", "path");
    }
    TraceResult result;
    result.path = std::move(*path_text);
    if (const JsonValue* span_count = value.find("spanCount"); span_count != nullptr) {
        const auto integer = span_count->as_integer();
        if (!integer.has_value() || *integer < 0) {
            return invalid_argument("spanCount must be a non-negative integer", "spanCount");
        }
        result.span_count = static_cast<std::uint64_t>(*integer);
    }
    return CommandResult{std::move(result)};
}

[[nodiscard]] std::expected<CommandResult, ProtocolError>
decode_audit_result(const JsonValue& value) {
    if (auto error = reject_unknown(value, {"profile", "path", "score", "passed"});
        error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    const JsonValue* profile = value.find("profile");
    auto profile_text = read_string(profile);
    if (profile == nullptr) {
        return invalid_argument("audit result must carry profile", "profile");
    }
    if (!profile_text.has_value() || profile_text->empty()) {
        return invalid_argument("profile must be a non-empty string", "profile");
    }
    AuditResult result;
    result.profile = std::move(*profile_text);
    if (const JsonValue* path = value.find("path"); path != nullptr) {
        auto path_text = read_string(path);
        if (!path_text.has_value()) {
            return invalid_argument("path must be a string", "path");
        }
        result.path = std::move(*path_text);
    }
    const JsonValue* score = value.find("score");
    const auto score_number = score == nullptr ? std::nullopt : score->as_number();
    if (!score_number.has_value() || !std::isfinite(*score_number)) {
        return invalid_argument("score must be a finite number", "score");
    }
    result.score = *score_number;
    const JsonValue* passed = value.find("passed");
    const auto passed_flag = passed == nullptr ? std::nullopt : passed->as_bool();
    if (!passed_flag.has_value()) {
        return invalid_argument("passed must be a boolean", "passed");
    }
    result.passed = *passed_flag;
    return CommandResult{std::move(result)};
}

} // namespace

std::expected<JsonValue, ProtocolError> encode_command_result(const CommandResult& result) {
    return std::visit(Overloaded{
                          [](const SnapshotResult& value) { return encode_snapshot_result(value); },
                          [](const ActionResult& value) { return encode_action_result(value); },
                          [](const TraceResult& value) { return encode_trace_result(value); },
                          [](const AuditResult& value) { return encode_audit_result(value); },
                      },
                      result);
}

std::expected<CommandResult, ProtocolError> decode_command_result(const JsonValue& value,
                                                                  std::string_view command) {
    if (command == kSnapshotCommand) {
        return decode_snapshot_result(value);
    }
    if (command == kTraceCommand) {
        return decode_trace_result(value);
    }
    if (command == kAuditCommand) {
        return decode_audit_result(value);
    }
    if (action_kind_from_string(command).has_value()) {
        return decode_action_result(value);
    }
    return std::unexpected(ProtocolError{ProtocolErrorCode::InvalidArgument,
                                         "result does not match a known command",
                                         std::string(command), std::nullopt});
}

} // namespace tessera::harness
