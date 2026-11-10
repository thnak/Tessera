// SPDX-License-Identifier: MIT

#include "session_test_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace st = session_test;

int main() {
    // A full session over a fake transport: one request of every v1 command, answered in request
    // order with the expected result shape.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        st::SessionExecutor session(*pair.second, handler, clock);

        tessera::harness::ActionCommand set_value;
        set_value.kind = tessera::harness::ActionKind::SetValue;
        set_value.ui_id = 10;
        set_value.value = "alice";
        set_value.item_key = std::string("row");
        st::write_lines(
            *pair.first,
            {st::request_line(1, st::SnapshotCommand{true}),
             st::request_line(
                 2,
                 tessera::harness::ActionCommand{
                     tessera::harness::ActionKind::Click, 9, std::nullopt, {}, 0.0, 0.0}),
             st::request_line(3, set_value),
             st::request_line(4, tessera::harness::TraceCommand{"out/trace.json"}),
             st::request_line(5, tessera::harness::AuditCommand{"desktop", ""})});

        const st::SessionPumpStats stats = session.pump();
        st::expect(stats.commands_dispatched == 5, "all five commands dispatched");
        st::expect(stats.requests_answered == 5, "all five answered");
        st::expect(stats.responses_written == 5, "all five written");
        st::expect(handler.seen.size() == 5, "handler saw every command");

        const auto snapshot = st::read_response(*pair.first, "snapshot response");
        st::expect(snapshot.has_value() && snapshot->id == 1 && snapshot->ok, "snapshot ok");
        if (snapshot.has_value()) {
            const auto decoded =
                tessera::harness::decode_command_result(snapshot->result, "snapshot");
            st::expect(decoded.has_value() && std::get<st::SnapshotResult>(*decoded).include_hidden,
                       "snapshot includeHidden echoed");
        }

        const auto clicked = st::read_response(*pair.first, "click response");
        st::expect(clicked.has_value() && clicked->id == 2 && clicked->ok, "click ok");
        if (clicked.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(clicked->result, "click");
            st::expect(decoded.has_value() && std::get<st::ActionResult>(*decoded).ui_id == 9,
                       "click uiId echoed");
        }

        const auto typed = st::read_response(*pair.first, "setValue response");
        st::expect(typed.has_value() && typed->ok, "setValue ok");
        if (typed.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(typed->result, "setValue");
            st::expect(decoded.has_value() && std::get<st::ActionResult>(*decoded).item_key ==
                                                  std::optional<std::string>("row"),
                       "setValue itemKey echoed");
        }
        st::expect(std::get<tessera::harness::ActionCommand>(handler.seen[2]).value == "alice",
                   "handler saw the setValue text");

        const auto traced = st::read_response(*pair.first, "trace response");
        st::expect(traced.has_value() && traced->ok, "trace ok");
        if (traced.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(traced->result, "trace");
            st::expect(decoded.has_value() &&
                           std::get<st::TraceResult>(*decoded).path == "out/trace.json",
                       "trace path echoed");
        }

        const auto audited = st::read_response(*pair.first, "audit response");
        st::expect(audited.has_value() && audited->ok, "audit ok");
        if (audited.has_value()) {
            const auto decoded = tessera::harness::decode_command_result(audited->result, "audit");
            st::expect(decoded.has_value() &&
                           std::get<tessera::harness::AuditResult>(*decoded).passed,
                       "audit verdict echoed");
        }
    }

    // Decode failures are answered in arrival order and the session stays usable.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        st::SessionExecutor session(*pair.second, handler, clock);
        st::write_lines(*pair.first, {"not json", "{\"id\":2,\"cmd\":\"nope\"}",
                                      "{\"id\":3,\"cmd\":\"click\",\"uiId\":\"-1\"}",
                                      "{\"id\":4,\"cmd\":\"snapshot\"}"});
        static_cast<void>(session.pump());

        const auto malformed = st::read_response(*pair.first, "malformed answer");
        st::expect(malformed.has_value() && !malformed->ok &&
                       malformed->error.code ==
                           tessera::harness::ProtocolErrorCode::MalformedRequest,
                   "malformed request answered");
        const auto unknown = st::read_response(*pair.first, "unknown command answer");
        st::expect(unknown.has_value() && !unknown->ok &&
                       unknown->error.code == tessera::harness::ProtocolErrorCode::UnknownCommand,
                   "unknown command answered");
        const auto invalid = st::read_response(*pair.first, "invalid argument answer");
        st::expect(invalid.has_value() && !invalid->ok &&
                       invalid->error.code == tessera::harness::ProtocolErrorCode::InvalidArgument,
                   "invalid argument answered");
        const auto valid = st::read_response(*pair.first, "valid answer");
        st::expect(valid.has_value() && valid->ok && valid->id == 4,
                   "valid request still answered");
        st::expect(handler.seen.size() == 1, "only the valid request reached the handler");
    }

    // A queued request whose deadline passed is answered with Timeout, and the session continues.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        tessera::harness::SessionLimits limits;
        limits.max_commands_per_pump = 1;
        limits.command_timeout_us = 100;
        st::SessionExecutor session(*pair.second, handler, clock, limits);
        st::write_lines(*pair.first, {st::request_line(1, st::SnapshotCommand{}),
                                      st::request_line(2, st::SnapshotCommand{})});
        const st::SessionPumpStats first = session.pump();
        st::expect(first.commands_dispatched == 1, "one command per pump");
        const auto on_time = st::read_response(*pair.first, "first answer");
        st::expect(on_time.has_value() && on_time->ok, "first on time");

        clock.now = 150;
        const st::SessionPumpStats second = session.pump();
        st::expect(second.commands_dispatched == 0, "expired request never reached the handler");
        st::expect(second.requests_answered == 1, "expired request answered");
        const auto late = st::read_response(*pair.first, "late answer");
        st::expect(late.has_value() && !late->ok &&
                       late->error.code == tessera::harness::ProtocolErrorCode::Timeout,
                   "queued command timed out");
        st::expect(late.has_value() && late->elapsed_us == 0,
                   "timed-out-before-dispatch elapsed is zero");

        st::write_lines(*pair.first, {st::request_line(3, st::SnapshotCommand{})});
        clock.now = 1000;
        static_cast<void>(session.pump());
        const auto recovered = st::read_response(*pair.first, "recovered answer");
        st::expect(recovered.has_value() && recovered->ok, "session remains usable");
    }

    // A handler that overruns its budget has its result replaced with Timeout.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        handler.clock = &clock;
        handler.advance_us = 200;
        tessera::harness::SessionLimits limits;
        limits.command_timeout_us = 100;
        st::SessionExecutor session(*pair.second, handler, clock, limits);
        st::write_lines(*pair.first, {st::request_line(1, st::SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto slow = st::read_response(*pair.first, "slow answer");
        st::expect(slow.has_value() && !slow->ok &&
                       slow->error.code == tessera::harness::ProtocolErrorCode::Timeout,
                   "slow handler timed out");
        st::expect(slow.has_value() && slow->elapsed_us == 200, "elapsed time reported");
        handler.advance_us = 0;
        st::write_lines(*pair.first, {st::request_line(2, st::SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto fast = st::read_response(*pair.first, "fast answer");
        st::expect(fast.has_value() && fast->ok, "fast command after a timeout");
    }

    // A host error keeps its typed code and correlation id.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        handler.forced_error = tessera::harness::ProtocolError{
            tessera::harness::ProtocolErrorCode::UiIdNotFound, "no node", "77", std::nullopt};
        st::SessionExecutor session(*pair.second, handler, clock);
        st::write_lines(*pair.first, {st::request_line(7, st::SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto failed = st::read_response(*pair.first, "host error");
        st::expect(failed.has_value() && failed->id == 7 && !failed->ok &&
                       failed->error.code == tessera::harness::ProtocolErrorCode::UiIdNotFound,
                   "host error propagated");
    }

    // The request queue is bounded: a full queue stops reading instead of dropping or growing.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        tessera::harness::SessionLimits limits;
        limits.max_queued_requests = 1;
        limits.max_commands_per_pump = 0;
        st::SessionExecutor session(*pair.second, handler, clock, limits);
        st::write_lines(*pair.first, {st::request_line(1, st::SnapshotCommand{}),
                                      st::request_line(2, st::SnapshotCommand{}),
                                      st::request_line(3, st::SnapshotCommand{})});
        const st::SessionPumpStats stats = session.pump();
        st::expect(stats.lines_read == 1, "only one line read at the queue cap");
        st::expect(stats.read_backpressure, "back-pressure reported");
        st::expect(stats.requests_answered == 0, "nothing answered while queued");
        st::expect(session.queued_requests() == 1, "queue holds one request");
        st::expect(stats.has_more_work, "more work remains");
    }

    // Shutdown cancels queued work in arrival order, then closes; the peer can still read every
    // diagnostic that was accepted.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        tessera::harness::SessionLimits limits;
        limits.max_commands_per_pump = 0;
        st::SessionExecutor session(*pair.second, handler, clock, limits);
        st::write_lines(*pair.first, {st::request_line(1, st::SnapshotCommand{}),
                                      st::request_line(2, st::SnapshotCommand{}),
                                      st::request_line(3, st::SnapshotCommand{})});
        static_cast<void>(session.pump());
        st::expect(session.queued_requests() == 3, "three requests in flight");
        const st::SessionPumpStats stats = session.shutdown();
        st::expect(stats.closed && session.closed(), "session closed by shutdown");
        for (int id = 1; id <= 3; ++id) {
            const auto cancelled = st::read_response(*pair.first, "cancellation answer");
            st::expect(cancelled.has_value() &&
                           cancelled->id == static_cast<tessera::harness::RequestId>(id) &&
                           !cancelled->ok &&
                           cancelled->error.code == tessera::harness::ProtocolErrorCode::Internal,
                       "queued request cancelled in order");
        }
        st::expect(pair.first->read_line().error().code ==
                       tessera::harness::TransportErrorCode::Closed,
                   "transport closed after the drain");
        st::expect(handler.seen.empty(), "cancelled commands never ran");
        st::expect(session.pump().closed, "pump after shutdown reports closed");
    }

    if (st::failures != 0) {
        return 1;
    }
    return 0;
}
