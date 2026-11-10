// SPDX-License-Identifier: MIT

#include "tessera/harness/in_process_transport.hpp"
#include "tessera/harness/session.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using tessera::harness::ActionResult;
using tessera::harness::CommandHandler;
using tessera::harness::CommandPayload;
using tessera::harness::CommandResult;
using tessera::harness::InProcessPair;
using tessera::harness::JsonValue;
using tessera::harness::LineTransport;
using tessera::harness::make_in_process_pair;
using tessera::harness::ProtocolError;
using tessera::harness::ProtocolErrorCode;
using tessera::harness::RequestId;
using tessera::harness::Response;
using tessera::harness::SessionClock;
using tessera::harness::SessionExecutor;
using tessera::harness::SessionLimits;
using tessera::harness::SessionPumpStats;
using tessera::harness::SnapshotCommand;
using tessera::harness::SnapshotResult;
using tessera::harness::TraceResult;
using tessera::harness::TransportErrorCode;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_session: " << message << "\n";
        ++failures;
    }
    return condition;
}

class ManualSessionClock final : public SessionClock {
public:
    [[nodiscard]] std::uint64_t now_us() const override {
        return now;
    }

    std::uint64_t now = 0;
};

class RecordingHandler final : public CommandHandler {
public:
    [[nodiscard]] std::expected<CommandResult, ProtocolError>
    handle(const CommandPayload& command) override {
        seen.push_back(command);
        if (clock != nullptr) {
            clock->now += advance_us;
        }
        if (forced_error.has_value()) {
            return std::unexpected(*forced_error);
        }
        return std::visit(
            [this](const auto& value) -> std::expected<CommandResult, ProtocolError> {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, SnapshotCommand>) {
                    if (oversized_snapshot_bytes != 0) {
                        return CommandResult{SnapshotResult{
                            value.include_hidden,
                            JsonValue::object({{"root", JsonValue::string(std::string(
                                                            oversized_snapshot_bytes, 'x'))}})}};
                    }
                    return CommandResult{SnapshotResult{value.include_hidden, snapshot}};
                } else if constexpr (std::is_same_v<Type, tessera::harness::ActionCommand>) {
                    return CommandResult{ActionResult{value.kind, value.ui_id, value.item_key}};
                } else if constexpr (std::is_same_v<Type, tessera::harness::TraceCommand>) {
                    return CommandResult{TraceResult{value.path, 2}};
                } else {
                    return CommandResult{
                        tessera::harness::AuditResult{value.profile, value.path, 90.0, true}};
                }
            },
            command);
    }

    ManualSessionClock* clock = nullptr;
    std::uint64_t advance_us = 0;
    std::optional<ProtocolError> forced_error;
    std::size_t oversized_snapshot_bytes = 0;
    JsonValue snapshot = JsonValue::object({{"root", JsonValue::object()}});
    std::vector<CommandPayload> seen;
};

std::string request_line(RequestId id, const CommandPayload& payload) {
    auto encoded = tessera::harness::encode_command(id, payload);
    if (!encoded.has_value()) {
        expect(false, "request encoding failed");
        return {};
    }
    return *encoded;
}

void write_lines(LineTransport& transport, const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        expect(transport.write_line(line).has_value(), "request line written");
    }
}

std::optional<Response> read_response(LineTransport& transport, const char* message) {
    auto line = transport.read_line();
    if (!line.has_value()) {
        expect(false, message);
        return std::nullopt;
    }
    auto response = tessera::harness::decode_response(*line);
    if (!response.has_value()) {
        expect(false, message);
        return std::nullopt;
    }
    return *response;
}

// Transport whose output can be stalled and whose input can reach EOF while output stays open,
// which the in-process pair cannot express (closing one endpoint closes both directions).
class StallingTransport final : public LineTransport {
public:
    [[nodiscard]] std::expected<std::string, tessera::harness::TransportError>
    read_line() override {
        if (next_inbound < inbound.size()) {
            return inbound[next_inbound++];
        }
        if (input_done) {
            return std::unexpected(
                tessera::harness::TransportError{TransportErrorCode::Closed, "input done"});
        }
        return std::unexpected(
            tessera::harness::TransportError{TransportErrorCode::WouldBlock, "no line"});
    }

    [[nodiscard]] std::expected<void, tessera::harness::TransportError>
    write_line(std::string_view line) override {
        if (closed_state) {
            return std::unexpected(
                tessera::harness::TransportError{TransportErrorCode::Closed, "closed"});
        }
        if (block_writes) {
            return std::unexpected(
                tessera::harness::TransportError{TransportErrorCode::WouldBlock, "stalled"});
        }
        outbound.emplace_back(line);
        return {};
    }

    [[nodiscard]] std::expected<void, tessera::harness::TransportError> flush() override {
        if (block_writes) {
            return std::unexpected(
                tessera::harness::TransportError{TransportErrorCode::WouldBlock, "stalled"});
        }
        return {};
    }

    [[nodiscard]] std::size_t max_line_bytes() const noexcept override {
        return 4096;
    }
    [[nodiscard]] bool closed() const noexcept override {
        return closed_state;
    }
    void close() noexcept override {
        closed_state = true;
    }

    std::vector<std::string> inbound;
    std::size_t next_inbound = 0;
    bool input_done = false;
    std::vector<std::string> outbound;
    bool block_writes = false;
    bool closed_state = false;
};

std::optional<Response> read_outbound(StallingTransport& transport) {
    if (transport.outbound.empty()) {
        expect(false, "expected an outbound response");
        return std::nullopt;
    }
    std::string line = std::move(transport.outbound.front());
    transport.outbound.erase(transport.outbound.begin());
    auto response = tessera::harness::decode_response(line);
    if (!response.has_value()) {
        expect(false, "outbound response did not decode");
        return std::nullopt;
    }
    return *response;
}

} // namespace

int main() {
    // A full session over a fake transport: one request of every v1 command, answered in request
    // order with the expected result shape.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionExecutor session(*pair.second, handler, clock);

        tessera::harness::ActionCommand set_value;
        set_value.kind = tessera::harness::ActionKind::SetValue;
        set_value.ui_id = 10;
        set_value.value = "alice";
        set_value.item_key = std::string("row");
        write_lines(
            *pair.first,
            {request_line(1, SnapshotCommand{true}),
             request_line(2,
                          tessera::harness::ActionCommand{
                              tessera::harness::ActionKind::Click, 9, std::nullopt, {}, 0.0, 0.0}),
             request_line(3, set_value),
             request_line(4, tessera::harness::TraceCommand{"out/trace.json"}),
             request_line(5, tessera::harness::AuditCommand{"desktop", ""})});

        const SessionPumpStats stats = session.pump();
        expect(stats.commands_dispatched == 5, "all five commands dispatched");
        expect(stats.requests_answered == 5, "all five answered");
        expect(stats.responses_written == 5, "all five written");
        expect(handler.seen.size() == 5, "handler saw every command");

        const auto snapshot = read_response(*pair.first, "snapshot response");
        expect(snapshot.has_value() && snapshot->id == 1 && snapshot->ok, "snapshot ok");
        if (snapshot.has_value()) {
            const auto decoded =
                tessera::harness::decode_command_result(snapshot->result, "snapshot");
            expect(decoded.has_value() && std::get<SnapshotResult>(*decoded).include_hidden,
                   "snapshot includeHidden echoed");
        }

        const auto clicked = read_response(*pair.first, "click response");
        expect(clicked.has_value() && clicked->id == 2 && clicked->ok, "click ok");
        if (clicked.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(clicked->result, "click");
            expect(decoded.has_value() && std::get<ActionResult>(*decoded).ui_id == 9,
                   "click uiId echoed");
        }

        const auto typed = read_response(*pair.first, "setValue response");
        expect(typed.has_value() && typed->ok, "setValue ok");
        if (typed.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(typed->result, "setValue");
            expect(decoded.has_value() && std::get<ActionResult>(*decoded).item_key ==
                                              std::optional<std::string>("row"),
                   "setValue itemKey echoed");
        }
        expect(std::get<tessera::harness::ActionCommand>(handler.seen[2]).value == "alice",
               "handler saw the setValue text");

        const auto traced = read_response(*pair.first, "trace response");
        expect(traced.has_value() && traced->ok, "trace ok");
        if (traced.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(traced->result, "trace");
            expect(decoded.has_value() && std::get<TraceResult>(*decoded).path == "out/trace.json",
                   "trace path echoed");
        }

        const auto audited = read_response(*pair.first, "audit response");
        expect(audited.has_value() && audited->ok, "audit ok");
        if (audited.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(audited->result, "audit");
            expect(decoded.has_value() && std::get<tessera::harness::AuditResult>(*decoded).passed,
                   "audit verdict echoed");
        }
    }

    // Decode failures are answered in arrival order and the session stays usable.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionExecutor session(*pair.second, handler, clock);
        write_lines(*pair.first, {"not json", "{\"id\":2,\"cmd\":\"nope\"}",
                                  "{\"id\":3,\"cmd\":\"click\",\"uiId\":\"-1\"}",
                                  "{\"id\":4,\"cmd\":\"snapshot\"}"});
        static_cast<void>(session.pump());

        const auto malformed = read_response(*pair.first, "malformed answer");
        expect(malformed.has_value() && !malformed->ok &&
                   malformed->error.code == ProtocolErrorCode::MalformedRequest,
               "malformed request answered");
        const auto unknown = read_response(*pair.first, "unknown command answer");
        expect(unknown.has_value() && !unknown->ok &&
                   unknown->error.code == ProtocolErrorCode::UnknownCommand,
               "unknown command answered");
        const auto invalid = read_response(*pair.first, "invalid argument answer");
        expect(invalid.has_value() && !invalid->ok &&
                   invalid->error.code == ProtocolErrorCode::InvalidArgument,
               "invalid argument answered");
        const auto valid = read_response(*pair.first, "valid answer");
        expect(valid.has_value() && valid->ok && valid->id == 4, "valid request still answered");
        expect(handler.seen.size() == 1, "only the valid request reached the handler");
    }

    // A queued request whose deadline passed is answered with Timeout, and the session continues.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionLimits limits;
        limits.max_commands_per_pump = 1;
        limits.command_timeout_us = 100;
        SessionExecutor session(*pair.second, handler, clock, limits);
        write_lines(*pair.first,
                    {request_line(1, SnapshotCommand{}), request_line(2, SnapshotCommand{})});
        const SessionPumpStats first = session.pump();
        expect(first.commands_dispatched == 1, "one command per pump");
        const auto on_time = read_response(*pair.first, "first answer");
        expect(on_time.has_value() && on_time->ok, "first on time");

        clock.now = 150;
        const SessionPumpStats second = session.pump();
        expect(second.commands_dispatched == 0, "expired request never reached the handler");
        expect(second.requests_answered == 1, "expired request answered");
        const auto late = read_response(*pair.first, "late answer");
        expect(late.has_value() && !late->ok && late->error.code == ProtocolErrorCode::Timeout,
               "queued command timed out");
        expect(late.has_value() && late->elapsed_us == 0,
               "timed-out-before-dispatch elapsed is zero");

        write_lines(*pair.first, {request_line(3, SnapshotCommand{})});
        clock.now = 1000;
        static_cast<void>(session.pump());
        const auto recovered = read_response(*pair.first, "recovered answer");
        expect(recovered.has_value() && recovered->ok, "session remains usable");
    }

    // A handler that overruns its budget has its result replaced with Timeout.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        handler.clock = &clock;
        handler.advance_us = 200;
        SessionLimits limits;
        limits.command_timeout_us = 100;
        SessionExecutor session(*pair.second, handler, clock, limits);
        write_lines(*pair.first, {request_line(1, SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto slow = read_response(*pair.first, "slow answer");
        expect(slow.has_value() && !slow->ok && slow->error.code == ProtocolErrorCode::Timeout,
               "slow handler timed out");
        expect(slow.has_value() && slow->elapsed_us == 200, "elapsed time reported");
        handler.advance_us = 0;
        write_lines(*pair.first, {request_line(2, SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto fast = read_response(*pair.first, "fast answer");
        expect(fast.has_value() && fast->ok, "fast command after a timeout");
    }

    // A host error keeps its typed code and correlation id.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        handler.forced_error =
            ProtocolError{ProtocolErrorCode::UiIdNotFound, "no node", "77", std::nullopt};
        SessionExecutor session(*pair.second, handler, clock);
        write_lines(*pair.first, {request_line(7, SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto failed = read_response(*pair.first, "host error");
        expect(failed.has_value() && failed->id == 7 && !failed->ok &&
                   failed->error.code == ProtocolErrorCode::UiIdNotFound,
               "host error propagated");
    }

    // The request queue is bounded: a full queue stops reading instead of dropping or growing.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionLimits limits;
        limits.max_queued_requests = 1;
        limits.max_commands_per_pump = 0;
        SessionExecutor session(*pair.second, handler, clock, limits);
        write_lines(*pair.first,
                    {request_line(1, SnapshotCommand{}), request_line(2, SnapshotCommand{}),
                     request_line(3, SnapshotCommand{})});
        const SessionPumpStats stats = session.pump();
        expect(stats.lines_read == 1, "only one line read at the queue cap");
        expect(stats.read_backpressure, "back-pressure reported");
        expect(stats.requests_answered == 0, "nothing answered while queued");
        expect(session.queued_requests() == 1, "queue holds one request");
        expect(stats.has_more_work, "more work remains");
    }

    // Shutdown cancels queued work in arrival order, then closes; the peer can still read every
    // diagnostic that was accepted.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionLimits limits;
        limits.max_commands_per_pump = 0;
        SessionExecutor session(*pair.second, handler, clock, limits);
        write_lines(*pair.first,
                    {request_line(1, SnapshotCommand{}), request_line(2, SnapshotCommand{}),
                     request_line(3, SnapshotCommand{})});
        static_cast<void>(session.pump());
        expect(session.queued_requests() == 3, "three requests in flight");
        const SessionPumpStats stats = session.shutdown();
        expect(stats.closed && session.closed(), "session closed by shutdown");
        for (int id = 1; id <= 3; ++id) {
            const auto cancelled = read_response(*pair.first, "cancellation answer");
            expect(cancelled.has_value() && cancelled->id == static_cast<RequestId>(id) &&
                       !cancelled->ok && cancelled->error.code == ProtocolErrorCode::Internal,
                   "queued request cancelled in order");
        }
        expect(pair.first->read_line().error().code == TransportErrorCode::Closed,
               "transport closed after the drain");
        expect(handler.seen.empty(), "cancelled commands never ran");
        expect(session.pump().closed, "pump after shutdown reports closed");
    }

    // Stalled output is bounded and retried in order; responses are not lost or reordered.
    {
        StallingTransport transport;
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionExecutor session(transport, handler, clock);
        transport.inbound.push_back(request_line(1, SnapshotCommand{}));
        transport.inbound.push_back(request_line(2, SnapshotCommand{}));
        transport.block_writes = true;
        const SessionPumpStats blocked = session.pump();
        expect(blocked.commands_dispatched == 2, "both commands ran");
        expect(blocked.responses_written == 0, "no response written while stalled");
        expect(blocked.output_blocked, "output back-pressure reported");
        transport.block_writes = false;
        const SessionPumpStats resumed = session.pump();
        expect(resumed.responses_written == 2, "both responses flushed on the next pump");
        const auto first = read_outbound(transport);
        const auto second = read_outbound(transport);
        expect(first.has_value() && first->id == 1, "first response first");
        expect(second.has_value() && second->id == 2, "second response second");
    }

    // Input EOF drains the queue, writes the answers, then closes the transport.
    {
        StallingTransport transport;
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionExecutor session(transport, handler, clock);
        transport.inbound.push_back(request_line(1, SnapshotCommand{}));
        transport.inbound.push_back(request_line(2, SnapshotCommand{}));
        transport.input_done = true;
        const SessionPumpStats stats = session.pump();
        expect(stats.input_closed, "eof observed");
        expect(stats.commands_dispatched == 2, "both commands ran at eof");
        expect(stats.closed && transport.closed(), "session closed after draining");
        expect(transport.outbound.size() == 2, "both answers written before close");
    }

    // An oversized response becomes a typed failure instead of an unwritable partial frame, and
    // the session keeps answering.
    {
        InProcessPair pair = make_in_process_pair(512);
        ManualSessionClock clock;
        RecordingHandler handler;
        handler.oversized_snapshot_bytes = 1024;
        SessionExecutor session(*pair.second, handler, clock);
        write_lines(*pair.first, {request_line(1, SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto too_big = read_response(*pair.first, "line-cap answer");
        expect(too_big.has_value() && too_big->id == 1 && !too_big->ok &&
                   too_big->error.code == ProtocolErrorCode::Internal &&
                   too_big->error.detail == "lineCap",
               "oversized response reported as a line-cap failure");
        handler.oversized_snapshot_bytes = 0;
        write_lines(*pair.first, {request_line(2, SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto follow_up = read_response(*pair.first, "follow-up answer");
        expect(follow_up.has_value() && follow_up->ok, "session usable after the cap");
    }

    // Decode errors and valid requests keep strict arrival order across several bounded pumps.
    {
        InProcessPair pair = make_in_process_pair();
        ManualSessionClock clock;
        RecordingHandler handler;
        SessionLimits limits;
        limits.max_reads_per_pump = 2;
        limits.max_commands_per_pump = 1;
        SessionExecutor session(*pair.second, handler, clock, limits);
        write_lines(*pair.first,
                    {"{\"id\":1,\"cmd\":\"nope\"}", request_line(2, SnapshotCommand{}),
                     "{\"id\":3,\"cmd\":\"click\"}", request_line(4, SnapshotCommand{}),
                     request_line(5, SnapshotCommand{})});
        std::vector<RequestId> answered;
        for (int pump = 0; pump < 12 && answered.size() < 5; ++pump) {
            static_cast<void>(session.pump());
            const auto response = read_response(*pair.first, "ordered answer");
            if (response.has_value()) {
                answered.push_back(response->id);
            }
        }
        const std::vector<RequestId> expected = {1, 2, 3, 4, 5};
        expect(answered == expected, "responses stay in request order across pumps");
    }

    if (failures != 0) {
        std::cerr << "harness_session: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
