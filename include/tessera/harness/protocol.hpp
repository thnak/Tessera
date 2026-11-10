// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/json_value.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::harness {

// Correlation id of one request/response pair. uint64 in C++, a decimal string on the wire: JSON
// numbers lose precision above 2^53 and automation ids routinely exceed that
// (docs/specs/automation-ids-v1.md section 1). A decoder also accepts a small JSON number so a
// hand-written client is not forced to quote ids.
using RequestId = std::uint64_t;

// Wire version of the line protocol, independent of the snapshot/script/audit schema versions.
// Envelopes always carry it on the way out; a request without it is treated as v1 so the examples
// in docs/specs/headless-automation-v1.md section 6 remain valid, while any other value is
// rejected with UnsupportedVersion rather than guessed at.
inline constexpr std::int64_t kProtocolVersion = 1;

// Typed failures shared by the envelope and the command model. The set is closed: a peer that
// sends an unknown code string decodes it as `Internal` and keeps the message, so a newer server
// degrades instead of crashing an older client.
enum class ProtocolErrorCode {
    MalformedRequest,   // not a JSON object, or a required envelope/command field is missing
    UnsupportedVersion, // protocolVersion is not kProtocolVersion
    UnknownCommand,     // cmd is not one of the v1 commands
    InvalidArgument,    // a command argument has the wrong type, value, or name
    UiIdNotFound,       // no live node matches the requested uiId
    Timeout,            // the step did not settle within its declared budget
    Internal,           // the harness failed; message/detail carry a diagnostic
};

struct ProtocolError {
    ProtocolErrorCode code = ProtocolErrorCode::Internal;
    std::string message; // human-readable, stable enough for logs
    std::string detail;  // optional machine-ish context: field name, offset, uiId
    // Correlation id recovered from the request before it failed, when one was readable. The
    // executor echoes it so a client can match the error envelope to its request.
    std::optional<RequestId> request_id;

    friend bool operator==(const ProtocolError&, const ProtocolError&) = default;
};

[[nodiscard]] std::string_view to_string(ProtocolErrorCode code) noexcept;
// Unknown strings map to ProtocolErrorCode::Internal, never to a value the caller did not send.
[[nodiscard]] ProtocolErrorCode protocol_error_code_from_string(std::string_view text) noexcept;

// Strict decimal text shared by correlation ids and automation UiIds: digits only, no sign, no
// leading zeros (except "0"), and no overflow. One id therefore has exactly one wire spelling, so a
// response cannot correlate with a different request than the client meant.
[[nodiscard]] bool parse_decimal_id(std::string_view text, std::uint64_t& out) noexcept;

// One decoded request. `arguments` holds every member of the line other than the envelope fields,
// in wire order: command parameters are flat on the wire (`{"id":2,"cmd":"snapshot",
// "includeHidden":true}`), matching the spec examples, and the command model owns their meaning.
struct Request {
    RequestId id = 0;
    std::string command; // wire command name, e.g. "snapshot" or "click"
    JsonValue arguments = JsonValue::object();

    friend bool operator==(const Request&, const Request&) = default;
};

// One response envelope. When `ok` is true `result` is the command result (null when the command
// has none); otherwise `error` describes the failure and `result` is ignored.
struct Response {
    RequestId id = 0;
    bool ok = true;
    JsonValue result;
    ProtocolError error;
    // Simulation microseconds the command took. The harness never reads a wall clock in
    // structural/golden modes (docs/specs/headless-automation-v1.md section 7), so this is the
    // fake-clock delta or 0 when the command had no clock.
    std::int64_t elapsed_us = 0;

    friend bool operator==(const Response&, const Response&) = default;
};

// Decodes one line (no terminator). Fails with MalformedRequest for bad JSON/shape,
// UnsupportedVersion for a future version, and echoes a recovered id on the error when possible.
[[nodiscard]] std::expected<Request, ProtocolError> decode_request(std::string_view line);

// Canonical request line without a terminator: `protocolVersion`, `id`, `cmd`, then the argument
// members in stored order. Arguments may not redefine an envelope field.
[[nodiscard]] std::expected<std::string, ProtocolError> encode_request(const Request& request);

// Decodes a response line. Strict about shape: an `ok` response may not carry `error`, a failure
// may not carry `result`, and unknown envelope fields are rejected instead of ignored (a new field
// is a version bump).
[[nodiscard]] std::expected<Response, ProtocolError> decode_response(std::string_view line);

// Canonical response line without a terminator. Field order is fixed: `protocolVersion`, `id`,
// `ok`, then `result` or `error`, then `elapsedUs`; an empty error detail is omitted.
[[nodiscard]] std::expected<std::string, ProtocolError> encode_response(const Response& response);

// Failure envelope for a request that could not be decoded or executed. Uses `error.request_id`
// when the decoder recovered one, otherwise the id the caller passed.
[[nodiscard]] Response make_error_response(RequestId id, ProtocolError error);

} // namespace tessera::harness
