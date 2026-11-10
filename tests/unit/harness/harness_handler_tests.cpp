// SPDX-License-Identifier: MIT

#include "tessera/harness/harness_handler.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::harness::ActionResult;
using tessera::harness::ArtifactSink;
using tessera::harness::AuditArtifact;
using tessera::harness::AuditCommand;
using tessera::harness::AuditResult;
using tessera::harness::AutomationHost;
using tessera::harness::FileArtifactSink;
using tessera::harness::HarnessCommandHandler;
using tessera::harness::HarnessHandlerConfig;
using tessera::harness::is_safe_artifact_path;
using tessera::harness::JsonValue;
using tessera::harness::ProtocolError;
using tessera::harness::ProtocolErrorCode;
using tessera::harness::SnapshotCommand;
using tessera::harness::SnapshotResult;
using tessera::harness::TraceArtifact;
using tessera::harness::TraceCommand;
using tessera::harness::TraceResult;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_handler: " << message << "\n";
        ++failures;
    }
    return condition;
}

ProtocolError make_error(ProtocolErrorCode code, std::string detail = {}) {
    return ProtocolError{code, "host error", std::move(detail), std::nullopt};
}

class FakeHost final : public AutomationHost {
public:
    [[nodiscard]] std::expected<JsonValue, ProtocolError>
    canonical_snapshot(bool include_hidden) override {
        ++snapshot_calls;
        last_include_hidden = include_hidden;
        if (snapshot_error.has_value()) {
            return std::unexpected(*snapshot_error);
        }
        return snapshot;
    }

    [[nodiscard]] std::expected<void, ProtocolError>
    dispatch_action(const tessera::harness::ActionCommand& action) override {
        actions.push_back(action);
        if (action_error.has_value()) {
            return std::unexpected(*action_error);
        }
        return {};
    }

    [[nodiscard]] std::expected<TraceArtifact, ProtocolError> collect_trace() override {
        ++trace_calls;
        if (trace_error.has_value()) {
            return std::unexpected(*trace_error);
        }
        return trace;
    }

    [[nodiscard]] std::expected<AuditArtifact, ProtocolError>
    run_audit(std::string_view profile) override {
        audit_profiles.emplace_back(profile);
        if (audit_error.has_value()) {
            return std::unexpected(*audit_error);
        }
        return audit;
    }

    JsonValue snapshot = JsonValue::object({{"root", JsonValue::object()}});
    TraceArtifact trace{"{\"traceEvents\":[]}", 3};
    AuditArtifact audit{"{\"score\":97}", 97.0, true};
    std::optional<ProtocolError> snapshot_error;
    std::optional<ProtocolError> action_error;
    std::optional<ProtocolError> trace_error;
    std::optional<ProtocolError> audit_error;
    std::vector<tessera::harness::ActionCommand> actions;
    std::vector<std::string> audit_profiles;
    bool last_include_hidden = false;
    int snapshot_calls = 0;
    int trace_calls = 0;
};

struct WrittenArtifact {
    std::string path;
    std::string bytes;
};

class FakeSink final : public ArtifactSink {
public:
    [[nodiscard]] std::expected<void, ProtocolError> write(std::string_view path,
                                                           std::string_view bytes) override {
        writes.push_back(WrittenArtifact{std::string(path), std::string(bytes)});
        if (write_error.has_value()) {
            return std::unexpected(*write_error);
        }
        return {};
    }

    std::vector<WrittenArtifact> writes;
    std::optional<ProtocolError> write_error;
};

} // namespace

int main() {
    // Artifact paths are sandboxed to a relative, conservative namespace.
    {
        expect(is_safe_artifact_path("out/trace.json"), "plain relative path");
        expect(is_safe_artifact_path("trace.json"), "bare file name");
        expect(!is_safe_artifact_path(""), "empty path rejected");
        expect(!is_safe_artifact_path("../escape.json"), "parent traversal rejected");
        expect(!is_safe_artifact_path("a/../b.json"), "inner traversal rejected");
        expect(!is_safe_artifact_path("a/./b.json"), "dot component rejected");
        expect(!is_safe_artifact_path("a//b.json"), "empty component rejected");
        expect(!is_safe_artifact_path("out/"), "trailing separator rejected");
        expect(!is_safe_artifact_path("/abs.json"), "absolute posix path rejected");
        expect(!is_safe_artifact_path("C:/abs.json"), "drive-qualified path rejected");
        expect(!is_safe_artifact_path("a\\b.json"), "backslash separator rejected");
        expect(!is_safe_artifact_path("a b.json"), "unsafe byte rejected");
        expect(!is_safe_artifact_path("a/nul\x01.json"), "control byte rejected");
        expect(!is_safe_artifact_path(std::string(600, 'a')), "over-long path rejected");
    }

    // Snapshot consumes the caller-provided canonical value and never recomputes identity.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink);
        const JsonValue expected = JsonValue::object(
            {{"root", JsonValue::object({{"uiId", JsonValue::string("18446744073709551615")}})}});
        host.snapshot = expected;
        const auto result = handler.handle(SnapshotCommand{true});
        expect(result.has_value(), "snapshot dispatched");
        const auto& snapshot = std::get<SnapshotResult>(*result);
        expect(snapshot.include_hidden, "includeHidden echoed");
        expect(snapshot.snapshot == expected, "snapshot payload passed through unchanged");
        expect(host.snapshot_calls == 1 && host.last_include_hidden, "host saw includeHidden");
        expect(sink.writes.empty(), "snapshot wrote no artifact");

        host.snapshot = JsonValue::string("not an object");
        const auto bad = handler.handle(SnapshotCommand{});
        expect(!bad.has_value() && bad.error().code == ProtocolErrorCode::Internal,
               "non-object snapshot rejected");
    }

    // Every action kind reaches the host sink and is echoed, including itemKey and payload fields.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink);
        tessera::harness::ActionCommand click;
        click.kind = tessera::harness::ActionKind::Click;
        click.ui_id = 42;
        const auto clicked = handler.handle(click);
        expect(clicked.has_value(), "click dispatched");
        expect(std::get<ActionResult>(*clicked).ui_id == 42, "click uiId echoed");

        tessera::harness::ActionCommand set_value;
        set_value.kind = tessera::harness::ActionKind::SetValue;
        set_value.ui_id = 7;
        set_value.value = "alice";
        set_value.item_key = std::string("row-1");
        const auto typed = handler.handle(set_value);
        expect(typed.has_value(), "setValue dispatched");
        expect(host.actions.size() == 2, "host saw both actions");
        expect(host.actions[1].value == "alice", "setValue payload forwarded");
        expect(host.actions[1].item_key == std::optional<std::string>("row-1"),
               "itemKey forwarded");
        expect(std::get<ActionResult>(*typed).item_key == std::optional<std::string>("row-1"),
               "itemKey echoed");

        tessera::harness::ActionCommand scroll;
        scroll.kind = tessera::harness::ActionKind::Scroll;
        scroll.scroll_dx = 0.0;
        scroll.scroll_dy = 120.0;
        expect(handler.handle(scroll).has_value(), "scroll dispatched");
        tessera::harness::ActionCommand focus;
        focus.kind = tessera::harness::ActionKind::Focus;
        expect(handler.handle(focus).has_value(), "focus dispatched");
        expect(host.actions[2].scroll_dy == 120.0, "scroll offset forwarded");

        host.action_error = make_error(ProtocolErrorCode::UiIdNotFound, "42");
        const auto missing = handler.handle(click);
        expect(!missing.has_value() && missing.error().code == ProtocolErrorCode::UiIdNotFound,
               "host action error propagates");
    }

    // Trace writes the host document under the requested path and echoes its span count.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink);
        const auto result = handler.handle(TraceCommand{"out/trace.json"});
        expect(result.has_value(), "trace dispatched");
        const auto& trace = std::get<TraceResult>(*result);
        expect(trace.path == "out/trace.json", "trace path echoed");
        expect(trace.span_count == 3, "span count echoed");
        expect(sink.writes.size() == 1 && sink.writes[0].path == "out/trace.json",
               "trace written to the requested path");
        expect(sink.writes[0].bytes == host.trace.json, "trace bytes written unchanged");
    }

    // Unsafe paths are rejected before the host is asked to produce a document.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink);
        const auto escaped = handler.handle(TraceCommand{"../trace.json"});
        expect(!escaped.has_value() && escaped.error().code == ProtocolErrorCode::InvalidArgument,
               "traversal trace path rejected");
        expect(host.trace_calls == 0 && sink.writes.empty(), "unsafe path reached no host");
        const auto absolute = handler.handle(AuditCommand{"desktop", "/tmp/audit.json"});
        expect(!absolute.has_value() && absolute.error().code == ProtocolErrorCode::InvalidArgument,
               "absolute audit path rejected");
        expect(host.audit_profiles.empty(), "unsafe audit path did not run the audit");
    }

    // The artifact size cap is enforced before the sink sees the bytes.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink, HarnessHandlerConfig{"out", 8});
        host.trace = TraceArtifact{"012345678", 0};
        const auto too_big = handler.handle(TraceCommand{"out/trace.json"});
        expect(!too_big.has_value() && too_big.error().code == ProtocolErrorCode::Internal,
               "oversized trace rejected");
        expect(sink.writes.empty(), "oversized trace never written");
        host.trace = TraceArtifact{"01234567", 1};
        expect(handler.handle(TraceCommand{"out/trace.json"}).has_value(),
               "artifact at the cap accepted");
        expect(sink.writes.size() == 1, "at-cap artifact written");
    }

    // An audit without a path lands on the section 5 default name; an explicit path is honored.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink);
        const auto defaulted = handler.handle(AuditCommand{"desktop", ""});
        expect(defaulted.has_value(), "audit dispatched");
        const auto& audit = std::get<AuditResult>(*defaulted);
        expect(audit.path == "out/audit.json", "audit default path");
        expect(audit.score == 97.0 && audit.passed, "audit verdict echoed");
        expect(host.audit_profiles.size() == 1 && host.audit_profiles[0] == "desktop",
               "profile forwarded");
        expect(sink.writes.size() == 1 && sink.writes[0].path == "out/audit.json",
               "audit default artifact written");

        const auto explicit_path = handler.handle(AuditCommand{"mobile", "out/mobile.json"});
        expect(explicit_path.has_value(), "explicit audit path accepted");
        expect(std::get<AuditResult>(*explicit_path).path == "out/mobile.json",
               "explicit audit path echoed");

        HarnessCommandHandler bare(host, sink, HarnessHandlerConfig{"", 1024});
        const auto no_directory = bare.handle(AuditCommand{"desktop", ""});
        expect(no_directory.has_value() &&
                   std::get<AuditResult>(*no_directory).path == "audit.json",
               "empty output directory uses the bare name");
    }

    // A host verdict outside the documented 0..100 range is a host fault, not a result.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink);
        host.audit.score = 101.0;
        const auto high = handler.handle(AuditCommand{"desktop", ""});
        expect(!high.has_value() && high.error().code == ProtocolErrorCode::Internal,
               "score above 100 rejected");
        host.audit.score = -1.0;
        expect(!handler.handle(AuditCommand{"desktop", ""}).has_value(), "negative score rejected");
        host.audit.score = 50.0;
        expect(handler.handle(AuditCommand{"desktop", ""}).has_value(), "score in range accepted");
    }

    // Host and sink failures keep their typed codes.
    {
        FakeHost host;
        FakeSink sink;
        HarnessCommandHandler handler(host, sink);
        host.trace_error = make_error(ProtocolErrorCode::Timeout, "trace");
        const auto timed_out = handler.handle(TraceCommand{"out/trace.json"});
        expect(!timed_out.has_value() && timed_out.error().code == ProtocolErrorCode::Timeout,
               "trace host error propagates");
        host.trace_error.reset();
        sink.write_error = make_error(ProtocolErrorCode::Internal, "sink");
        const auto failed = handler.handle(TraceCommand{"out/trace.json"});
        expect(!failed.has_value() && failed.error().code == ProtocolErrorCode::Internal,
               "sink error propagates");
    }

    // The filesystem sink refuses unsafe or oversized input and writes a valid path.
    {
        FileArtifactSink sink(8);
        expect(sink.write("../escape.json", "x").error().code == ProtocolErrorCode::InvalidArgument,
               "file sink rejects traversal");
        expect(sink.write("out/ok.json", "123456789").error().code == ProtocolErrorCode::Internal,
               "file sink enforces the cap");

        const std::filesystem::path root = "harness_artifact_test";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        expect(sink.write("harness_artifact_test/out/trace.json", "{\"ok\":1}").has_value(),
               "file sink writes a nested relative path");
        std::string bytes;
        {
            // The stream must be closed before the directory is removed: Windows refuses to
            // delete an open file.
            std::ifstream stream(root / "out" / "trace.json", std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        expect(bytes == "{\"ok\":1}", "file sink wrote the exact bytes");
        std::filesystem::remove_all(root, ignored);
    }

    if (failures != 0) {
        std::cerr << "harness_handler: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
