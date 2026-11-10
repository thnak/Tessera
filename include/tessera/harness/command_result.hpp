// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/command.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace tessera::harness {

// Result of a snapshot command. The tree payload is opaque to the codec: it is the canonical
// snapshot document produced by the automation serializer
// (docs/specs/automation-ids-v1.md section 5.2), embedded as a value so the harness never owns a
// second copy of the snapshot schema and never has to re-parse a serialized document.
struct SnapshotResult {
    bool include_hidden = false;
    JsonValue snapshot; // canonical UiSnapshot object

    friend bool operator==(const SnapshotResult&, const SnapshotResult&) = default;
};

// Echo of a dispatched input action. Actions go through the normal input path rather than mutating
// state directly, so the result records what was dispatched, not a UI diff.
struct ActionResult {
    ActionKind kind = ActionKind::Click;
    UiId ui_id = 0;
    std::optional<std::string> item_key;

    friend bool operator==(const ActionResult&, const ActionResult&) = default;
};

// Trace document written to `path`; `span_count` is the number of spans emitted.
struct TraceResult {
    std::string path;
    std::uint64_t span_count = 0;

    friend bool operator==(const TraceResult&, const TraceResult&) = default;
};

// Audit verdict for one profile; `path` is empty when the report went to the run's output
// directory. `score` is the profile's 0..100 score.
struct AuditResult {
    std::string profile;
    std::string path;
    double score = 0.0;
    bool passed = false;

    friend bool operator==(const AuditResult&, const AuditResult&) = default;
};

using CommandResult = std::variant<SnapshotResult, ActionResult, TraceResult, AuditResult>;

// Canonical result object: fixed field order per kind, ids as decimal strings, no insignificant
// whitespace. An inconsistent result (empty trace path, non-finite score, a snapshot payload that
// is not an object) is rejected instead of encoded.
[[nodiscard]] std::expected<JsonValue, ProtocolError>
encode_command_result(const CommandResult& result);

// Decodes a result object for `command` (the request command name). Results carry no
// discriminator of their own, and the executor always knows which command it answered, so the
// command name selects the shape and must agree with the payload.
[[nodiscard]] std::expected<CommandResult, ProtocolError>
decode_command_result(const JsonValue& value, std::string_view command);

} // namespace tessera::harness
