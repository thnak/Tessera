// SPDX-License-Identifier: MIT
//
// The scripted stdio stream and the transcript helpers shared by the MCP stdio suites. The stream's
// reads, stalls, and EOF are scripted, so no test depends on a real handle or a clock; it mirrors
// the fake in tests/unit/harness/stdio_transport_tests.cpp, which is the harness's own contract.

#pragma once

#include "mcp_test_support.hpp"
#include "serve_stdio.hpp"
#include "tessera/harness/stdio_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcp_stdio_test {

namespace harness = tessera::harness;
namespace mcp = tessera::mcp;

class ScriptedStream final : public harness::StdioStream {
public:
    [[nodiscard]] harness::StdioIoResult read_some(std::span<char> buffer) override {
        if (read_position == input.size()) {
            // Every byte has been consumed; whether that is EOF or a stall is the script's choice.
            return input_complete ? harness::StdioIoResult{harness::StdioIoStatus::EndOfStream, 0}
                                  : harness::StdioIoResult{harness::StdioIoStatus::WouldBlock, 0};
        }
        const std::size_t count = std::min(buffer.size(), input.size() - read_position);
        std::memcpy(buffer.data(), input.data() + read_position, count);
        read_position += count;
        return {harness::StdioIoStatus::Ok, count};
    }

    [[nodiscard]] harness::StdioIoResult write_some(std::string_view bytes) override {
        written.append(bytes);
        return {harness::StdioIoStatus::Ok, bytes.size()};
    }

    [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) override {
        ++wait_calls;
        last_wait = timeout;
        if (on_wait) {
            on_wait();
        }
        return wait_result;
    }

    std::string input;
    std::size_t read_position = 0;
    bool input_complete = true;
    bool wait_result = true;
    std::size_t wait_calls = 0;
    std::chrono::milliseconds last_wait{-1};
    std::string written;
    // Runs inside each readiness wait, so a test can observe idling (and stop it) without a thread.
    std::function<void()> on_wait;

    // The framed response lines the server wrote, in order.
    [[nodiscard]] std::vector<std::string> lines() const {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start < written.size()) {
            const std::size_t end = written.find('\n', start);
            if (end == std::string::npos) {
                break; // an unterminated tail is not a frame
            }
            out.emplace_back(written.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }
};

struct Fixture {
    Fixture()
        : stream_owner(std::make_unique<ScriptedStream>()),
          stream(*stream_owner),
          transport(std::move(stream_owner)) {}

    ScriptedStream* operator->() noexcept {
        return &stream;
    }

    std::unique_ptr<ScriptedStream> stream_owner;
    ScriptedStream& stream;
    harness::StdioTransport transport;
};

// One protocol line, with the LF a client writes, so a test reads as the transcript it asserts on.
inline std::string line(std::string_view json) {
    return std::string(json) + "\n";
}

// Runs a transcript through the loop and returns the response lines.
inline std::vector<std::string> exchange(Fixture& fixture, mcp::McpSession& session,
                                         std::string_view who) {
    const mcp::ServeResult result = mcp::serve_stdio(fixture.transport, session);
    mcp_test::expect(!result.failed, std::string(who) + ": the session completed cleanly");
    return fixture.stream.lines();
}

} // namespace mcp_stdio_test
