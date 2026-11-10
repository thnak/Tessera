// SPDX-License-Identifier: MIT

#include "tessera/harness/command_result.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_write.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

using tessera::harness::ActionKind;
using tessera::harness::ActionResult;
using tessera::harness::AuditResult;
using tessera::harness::CommandResult;
using tessera::harness::decode_command_result;
using tessera::harness::encode_command_result;
using tessera::harness::JsonValue;
using tessera::harness::parse_json;
using tessera::harness::ProtocolError;
using tessera::harness::ProtocolErrorCode;
using tessera::harness::SnapshotResult;
using tessera::harness::TraceResult;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_command_result: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string result_text(const CommandResult& result) {
    const auto encoded = encode_command_result(result);
    if (!encoded.has_value()) {
        return "<encode failed>";
    }
    const auto written = tessera::harness::to_json(*encoded);
    return written.has_value() ? *written : std::string("<write failed>");
}

std::optional<ProtocolErrorCode> decode_error(std::string_view text, std::string_view command) {
    const auto parsed = parse_json(text);
    if (!parsed.has_value()) {
        return ProtocolErrorCode::MalformedRequest;
    }
    const auto result = decode_command_result(*parsed, command);
    if (result.has_value()) {
        return std::nullopt;
    }
    return result.error().code;
}

void check_encoding() {
    const auto tree = parse_json(R"({"snapshotVersion":1,"sequence":"7","nodes":[]})");
    SnapshotResult snapshot;
    snapshot.include_hidden = true;
    snapshot.snapshot = *tree;
    expect(
        result_text(snapshot) ==
            R"({"snapshot":{"snapshotVersion":1,"sequence":"7","nodes":[]},"includeHidden":true})",
        "snapshot result bytes");

    ActionResult action;
    action.kind = ActionKind::Click;
    action.ui_id = 9;
    action.item_key = "row-3";
    expect(result_text(action) == R"({"action":"click","uiId":"9","itemKey":"row-3"})",
           "action result bytes");

    TraceResult trace;
    trace.path = "out/trace.json";
    trace.span_count = 12;
    expect(result_text(trace) == R"({"path":"out/trace.json","spanCount":12})",
           "trace result bytes");

    AuditResult audit;
    audit.profile = "desktop";
    audit.path = "out/audit.json";
    audit.score = 92.5;
    audit.passed = true;
    expect(result_text(audit) ==
               R"({"profile":"desktop","path":"out/audit.json","score":92.5,"passed":true})",
           "audit result bytes");

    AuditResult default_path;
    default_path.profile = "desktop";
    default_path.score = 88.0;
    expect(result_text(default_path) == R"({"profile":"desktop","score":88,"passed":false})",
           "empty audit path omitted");
}

void check_round_trips() {
    const auto tree = parse_json(R"({"snapshotVersion":1,"nodes":[{"uiId":"1"}]})");
    SnapshotResult snapshot;
    snapshot.snapshot = *tree;
    ActionResult action;
    action.kind = ActionKind::Scroll;
    action.ui_id = std::numeric_limits<std::uint64_t>::max();
    TraceResult trace;
    trace.path = "trace.json";
    trace.span_count = 3;
    AuditResult audit;
    audit.profile = "mobile";
    audit.score = 100.0;
    audit.passed = true;

    const CommandResult results[] = {snapshot, action, trace, audit};
    const char* names[] = {"snapshot", "scroll", "trace", "audit"};
    for (std::size_t index = 0; index < 4; ++index) {
        const auto encoded = encode_command_result(results[index]);
        if (!expect(encoded.has_value(), "result encode")) {
            continue;
        }
        const auto decoded = decode_command_result(*encoded, names[index]);
        if (!expect(decoded.has_value(), "result decode")) {
            continue;
        }
        expect(*decoded == results[index], "result round-trip");
    }

    // The command name selects the shape, so a mismatched name is an error rather than a guess.
    const auto encoded = encode_command_result(snapshot);
    expect(decode_command_result(*encoded, "trace").error().code ==
               ProtocolErrorCode::InvalidArgument,
           "mismatched result command rejected");
    expect(decode_command_result(*encoded, "teleport").error().code ==
               ProtocolErrorCode::InvalidArgument,
           "unknown result command rejected");
}

void check_rejections() {
    expect(decode_error(R"({"snapshot":[]})", "snapshot") == ProtocolErrorCode::InvalidArgument,
           "snapshot payload must be an object");
    expect(decode_error(R"({"includeHidden":true})", "snapshot") ==
               ProtocolErrorCode::InvalidArgument,
           "snapshot result requires the tree");
    expect(decode_error(R"({"snapshot":{},"extra":1})", "snapshot") ==
               ProtocolErrorCode::InvalidArgument,
           "unknown snapshot result field");
    expect(decode_error(R"({"snapshot":{},"includeHidden":"yes"})", "snapshot") ==
               ProtocolErrorCode::InvalidArgument,
           "includeHidden must be a boolean");
    expect(decode_error(R"({"action":"teleport","uiId":"1"})", "click") ==
               ProtocolErrorCode::InvalidArgument,
           "unknown action kind");
    expect(decode_error(R"({"action":"click"})", "click") == ProtocolErrorCode::InvalidArgument,
           "action result requires uiId");
    expect(decode_error(R"({"path":""})", "trace") == ProtocolErrorCode::InvalidArgument,
           "empty trace path rejected");
    expect(decode_error(R"({"path":"t.json","spanCount":-1})", "trace") ==
               ProtocolErrorCode::InvalidArgument,
           "negative span count rejected");
    expect(decode_error(R"({"profile":"desktop","score":1})", "audit") ==
               ProtocolErrorCode::InvalidArgument,
           "audit result requires passed");
    expect(decode_error(R"({"profile":"desktop","score":"high","passed":true})", "audit") ==
               ProtocolErrorCode::InvalidArgument,
           "audit score must be numeric");
    expect(decode_error(R"({"profile":"","score":1,"passed":true})", "audit") ==
               ProtocolErrorCode::InvalidArgument,
           "audit profile must not be empty");

    // Encoding refuses results that could not be decoded again.
    TraceResult empty_trace;
    expect(encode_command_result(empty_trace).error().code == ProtocolErrorCode::InvalidArgument,
           "empty trace path cannot be encoded");
    TraceResult huge_trace;
    huge_trace.path = "t.json";
    huge_trace.span_count = std::numeric_limits<std::uint64_t>::max();
    expect(encode_command_result(huge_trace).error().code == ProtocolErrorCode::InvalidArgument,
           "span count out of range");
    AuditResult infinite_score;
    infinite_score.profile = "desktop";
    infinite_score.score = std::numeric_limits<double>::infinity();
    expect(encode_command_result(infinite_score).error().code == ProtocolErrorCode::InvalidArgument,
           "non-finite score cannot be encoded");
    SnapshotResult non_object;
    non_object.snapshot = JsonValue::array();
    expect(encode_command_result(non_object).error().code == ProtocolErrorCode::InvalidArgument,
           "non-object snapshot cannot be encoded");
}

} // namespace

int main() {
    check_encoding();
    check_round_trips();
    check_rejections();
    if (failures != 0) {
        std::cerr << "harness_command_result: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
