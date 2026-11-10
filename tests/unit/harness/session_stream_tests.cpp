// SPDX-License-Identifier: MIT

#include "session_test_support.hpp"

#include <cstdint>
#include <vector>

namespace st = session_test;

int main() {
    // Stalled output is bounded and retried in order; responses are not lost or reordered.
    {
        st::StallingTransport transport;
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        st::SessionExecutor session(transport, handler, clock);
        transport.inbound.push_back(st::request_line(1, st::SnapshotCommand{}));
        transport.inbound.push_back(st::request_line(2, st::SnapshotCommand{}));
        transport.block_writes = true;
        const st::SessionPumpStats blocked = session.pump();
        st::expect(blocked.commands_dispatched == 2, "both commands ran");
        st::expect(blocked.responses_written == 0, "no response written while stalled");
        st::expect(blocked.output_blocked, "output back-pressure reported");
        transport.block_writes = false;
        const st::SessionPumpStats resumed = session.pump();
        st::expect(resumed.responses_written == 2, "both responses flushed on the next pump");
        const auto first = st::read_outbound(transport);
        const auto second = st::read_outbound(transport);
        st::expect(first.has_value() && first->id == 1, "first response first");
        st::expect(second.has_value() && second->id == 2, "second response second");
    }

    // Input EOF drains the queue, writes the answers, then closes the transport.
    {
        st::StallingTransport transport;
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        st::SessionExecutor session(transport, handler, clock);
        transport.inbound.push_back(st::request_line(1, st::SnapshotCommand{}));
        transport.inbound.push_back(st::request_line(2, st::SnapshotCommand{}));
        transport.input_done = true;
        const st::SessionPumpStats stats = session.pump();
        st::expect(stats.input_closed, "eof observed");
        st::expect(stats.commands_dispatched == 2, "both commands ran at eof");
        st::expect(stats.closed && transport.closed(), "session closed after draining");
        st::expect(transport.outbound.size() == 2, "both answers written before close");
    }

    // An oversized response becomes a typed failure instead of an unwritable partial frame, and
    // the session keeps answering.
    {
        st::InProcessPair pair = st::make_in_process_pair(512);
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        handler.oversized_snapshot_bytes = 1024;
        st::SessionExecutor session(*pair.second, handler, clock);
        st::write_lines(*pair.first, {st::request_line(1, st::SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto too_big = st::read_response(*pair.first, "line-cap answer");
        st::expect(too_big.has_value() && too_big->id == 1 && !too_big->ok &&
                       too_big->error.code == tessera::harness::ProtocolErrorCode::Internal &&
                       too_big->error.detail == "lineCap",
                   "oversized response reported as a line-cap failure");
        handler.oversized_snapshot_bytes = 0;
        st::write_lines(*pair.first, {st::request_line(2, st::SnapshotCommand{})});
        static_cast<void>(session.pump());
        const auto follow_up = st::read_response(*pair.first, "follow-up answer");
        st::expect(follow_up.has_value() && follow_up->ok, "session usable after the cap");
    }

    // Decode errors and valid requests keep strict arrival order across several bounded pumps.
    {
        st::InProcessPair pair = st::make_in_process_pair();
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        tessera::harness::SessionLimits limits;
        limits.max_reads_per_pump = 2;
        limits.max_commands_per_pump = 1;
        st::SessionExecutor session(*pair.second, handler, clock, limits);
        st::write_lines(*pair.first,
                        {"{\"id\":1,\"cmd\":\"nope\"}", st::request_line(2, st::SnapshotCommand{}),
                         "{\"id\":3,\"cmd\":\"click\"}", st::request_line(4, st::SnapshotCommand{}),
                         st::request_line(5, st::SnapshotCommand{})});
        std::vector<tessera::harness::RequestId> answered;
        for (int pump = 0; pump < 12 && answered.size() < 5; ++pump) {
            static_cast<void>(session.pump());
            const auto response = st::read_response(*pair.first, "ordered answer");
            if (response.has_value()) {
                answered.push_back(response->id);
            }
        }
        const std::vector<tessera::harness::RequestId> expected = {1, 2, 3, 4, 5};
        st::expect(answered == expected, "responses stay in request order across pumps");
    }

    if (st::failures != 0) {
        return 1;
    }
    return 0;
}
