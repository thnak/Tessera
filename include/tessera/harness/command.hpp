// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/json_value.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace tessera::harness {

// v1 command surface (docs/specs/headless-automation-v1.md section 6). The wire `cmd` is the
// action name itself, not a family wrapper: `{"id":3,"cmd":"click","uiId":"..."}`. Exactly these
// names exist in v1 and their parameters are flat members of the request object. Anything else is
// rejected (`unknownCommand`), and an unknown parameter is `invalidArgument` rather than silently
// ignored: an explicit `protocolVersion` bump is how a new field gets added.
inline constexpr std::string_view kSnapshotCommand = "snapshot";
inline constexpr std::string_view kClickCommand = "click";
inline constexpr std::string_view kSetValueCommand = "setValue";
inline constexpr std::string_view kFocusCommand = "focus";
inline constexpr std::string_view kScrollCommand = "scroll";
inline constexpr std::string_view kTraceCommand = "trace";
inline constexpr std::string_view kAuditCommand = "audit";

// Automation identity is a uint64 in C++, a decimal string on the wire (section 6; JavaScript loses
// precision above 2^53). The harness only carries the value: computing UiIds is the automation-id
// lane's contract (docs/specs/automation-ids-v1.md), and this codec never hashes anything.
using UiId = std::uint64_t;

enum class ActionKind {
    Click,
    SetValue,
    Focus,
    Scroll,
};

// Wire spellings of the action kinds; also the `cmd` names.
[[nodiscard]] std::string_view to_string(ActionKind kind) noexcept;
[[nodiscard]] std::optional<ActionKind> action_kind_from_string(std::string_view text) noexcept;

// Read-only structural query. Hidden/offscreen nodes are omitted with their subtree unless
// includeHidden is set (automation-ids-v1 section 5.2).
struct SnapshotCommand {
    bool include_hidden = false;

    friend bool operator==(const SnapshotCommand&, const SnapshotCommand&) = default;
};

// One input action against a UiId, optionally narrowed to a list/grid item by (UiId, itemKey).
// `value` carries setValue text, `scroll_dx`/`scroll_dy` carry scroll offsets; the encoder writes
// only the fields the kind uses, so an unused field can never reach the wire.
struct ActionCommand {
    ActionKind kind = ActionKind::Click;
    UiId ui_id = 0;
    std::optional<std::string> item_key;
    std::string value;
    double scroll_dx = 0.0;
    double scroll_dy = 0.0;

    friend bool operator==(const ActionCommand&, const ActionCommand&) = default;
};

// Write a Chrome/Perfetto-compatible trace document to `path`.
struct TraceCommand {
    std::string path;

    friend bool operator==(const TraceCommand&, const TraceCommand&) = default;
};

// Run a Lighthouse-style audit profile; `path` is empty when the audit shares the run's output
// directory.
struct AuditCommand {
    std::string profile;
    std::string path;

    friend bool operator==(const AuditCommand&, const AuditCommand&) = default;
};

using CommandPayload = std::variant<SnapshotCommand, ActionCommand, TraceCommand, AuditCommand>;

// The wire name of a decoded command; derived from the payload so name and payload can never
// disagree.
[[nodiscard]] std::string_view command_name(const CommandPayload& payload) noexcept;

// Decodes the arguments of one request into a typed command. `arguments` must be an object; a
// command with no parameters still takes an empty one. Missing or ill-typed parameters are
// InvalidArgument with the field name in `detail`; an unrecognized command name is
// UnknownCommand.
[[nodiscard]] std::expected<CommandPayload, ProtocolError>
decode_command(std::string_view name, const JsonValue& arguments);

// Convenience overload for a decoded request envelope.
[[nodiscard]] std::expected<CommandPayload, ProtocolError> decode_command(const Request& request);

// Canonical argument object for a command, with a fixed field order per kind. A command whose
// handler fields are inconsistent (for example a setValue with no value) is rejected here too, so
// an invalid command cannot be encoded.
[[nodiscard]] std::expected<JsonValue, ProtocolError>
encode_command_arguments(const CommandPayload& command);

// Full request line for `id` and `command`, ready for a line transport.
[[nodiscard]] std::expected<std::string, ProtocolError>
encode_command(RequestId id, const CommandPayload& command);

} // namespace tessera::harness
