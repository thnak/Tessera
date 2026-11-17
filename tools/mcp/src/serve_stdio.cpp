// SPDX-License-Identifier: MIT

#include "mcp_protocol.hpp"
#include "serve_stdio.hpp"
#include "tessera/harness/line_transport.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tessera::mcp {

namespace harness = tessera::harness;

namespace {

// The answer to a frame the transport refused (over-long, bad UTF-8, empty). The bytes were already
// dropped by the framing layer, so there is no request to correlate with: JSON-RPC's parse error
// with a null id is the only honest reply, and the `data.code` keeps it branchable.
constexpr std::string_view kRejectedFrameReply =
    R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"the line is not a frame",)"
    R"("data":{"code":"TESSERA_MCP_INVALID_PARAMS"}}})";

// True when the write failed only because the peer has not drained stdout yet. Those are retried
// after a short sleep; every other write failure means the stream is gone and the loop ends.
bool retryable(harness::TransportErrorCode code) noexcept {
    return code == harness::TransportErrorCode::QueueFull ||
           code == harness::TransportErrorCode::WouldBlock;
}

} // namespace

ServeResult serve_stdio(harness::LineTransport& transport, McpSession& session,
                        ServeOptions options) {
    ServeResult result;

    while (!transport.closed()) {
        if (options.stop != nullptr && options.stop->load(std::memory_order_acquire)) {
            result.stopped = true;
            return result;
        }

        auto line = transport.read_line();
        if (!line) {
            switch (line.error().code) {
            case harness::TransportErrorCode::WouldBlock:
                // Nothing to do, so idle on the bounded readiness wait instead of spinning. The
                // wait never consumes input and never exceeds the slice, so `stop` stays
                // observable.
                (void)transport.wait_readable(options.wait_slice);
                continue;
            case harness::TransportErrorCode::Closed:
                return result;
            case harness::TransportErrorCode::QueueFull:
                // Only a transport with a bounded read queue reports this; waiting briefly is the
                // same policy as a blocked write.
                std::this_thread::sleep_for(options.output_blocked_sleep);
                continue;
            case harness::TransportErrorCode::LineTooLong:
            case harness::TransportErrorCode::InvalidUtf8:
            case harness::TransportErrorCode::EmptyLine:
            case harness::TransportErrorCode::EmbeddedTerminator:
            case harness::TransportErrorCode::IoError: {
                ++result.rejected_frames;
                auto written = transport.write_line(kRejectedFrameReply);
                if (!written && !retryable(written.error().code)) {
                    result.failed = true;
                    return result;
                }
                if (written) {
                    ++result.lines_written;
                }
                continue;
            }
            }
            continue;
        }

        ++result.lines_read;
        auto reply = session.handle_line(*line);
        if (!reply) {
            // The session could not encode its answer. Ending is the only correct choice:
            // continuing would silently drop a request the client is waiting on.
            result.failed = true;
            return result;
        }
        // A notification is not answered (JSON-RPC forbids it); the session may still have lines
        // of its own to send, such as the `roots/list` request.
        std::vector<std::string_view> lines;
        if (reply->answered) {
            lines.push_back(reply->reply);
        }
        for (const std::string& outbound : reply->outbound) {
            lines.push_back(outbound);
        }
        for (const std::string_view text : lines) {
            auto written = transport.write_line(text);
            if (!written && !retryable(written.error().code)) {
                result.failed = true;
                return result;
            }
            if (written) {
                ++result.lines_written;
            }
        }
    }
    return result;
}

} // namespace tessera::mcp
