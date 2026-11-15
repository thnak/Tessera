// SPDX-License-Identifier: MIT

// The other half of the stdio loop's contract: a `tools/call` across the wire, and the loop's own
// boundedness -- it must idle on the readiness wait rather than spin, and it must stop when the
// flag says so. Split from `mcp_stdio` so each suite covers one concern.

#include "mcp_test_support.hpp"
#include "session.hpp"
#include "stdio_test_support.hpp"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mcp = tessera::mcp;
namespace harness = tessera::harness;
using mcp_stdio_test::exchange;
using mcp_stdio_test::Fixture;
using mcp_stdio_test::line;
using mcp_test::decode;
using mcp_test::expect;
using mcp_test::string_at;

constexpr std::string_view kHandshake =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"studio","version":"0.1"}}})";

void test_tools_call_round_trip() {
    Fixture fixture;
    fixture->input =
        line(kHandshake) +
        line(
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"diagnostics","arguments":{"source":"export function Clean() {\n  return <View />;\n}\n","file":"ui/Clean.tsx"}}})");
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_call");
    expect(responses.size() == 2, "mcp_call: the handshake and the call are both answered");
    if (responses.size() != 2) {
        return;
    }
    const harness::JsonValue response = decode(responses[1], "mcp_call");
    expect(response.find("error") == nullptr, "mcp_call: a valid call is not a protocol error");
    const harness::JsonValue* result = response.find("result");
    expect(result != nullptr, "mcp_call: the call answered with a result");
    if (result == nullptr) {
        return;
    }
    const harness::JsonValue* content = result->find("content");
    const harness::JsonValue::Array* items = content == nullptr ? nullptr : content->as_array();
    expect(items != nullptr && items->size() == 1, "mcp_call: the result has one content block");
    if (items == nullptr || items->empty()) {
        return;
    }
    expect(string_at(items->front(), "type") == "text", "mcp_call: the content block is text");
    // The tool result's text is itself the diagnostics document: that nesting is the MCP contract
    // (one text block per result) around the compiler-diagnostics-v1 document.
    const harness::JsonValue document = decode(string_at(items->front(), "text"), "mcp_call");
    expect(string_at(document, "file") == "ui/Clean.tsx",
           "mcp_call: the document names the project file the client sent");
    expect(document.find("diagnosticsVersion") != nullptr,
           "mcp_call: the document is the versioned diagnostics artifact");
}

void test_idle_waits_instead_of_spinning() {
    // A silent but open client must make the loop wait on the transport's bounded readiness seam,
    // not read in a tight loop. The scripted stream reports "not ready" and only becomes complete
    // from inside a wait, so the loop can only finish by waiting: a busy-poll implementation would
    // spin here until the test timed out instead.
    Fixture fixture;
    fixture->input = line(kHandshake);
    fixture->input_complete = false;
    fixture->wait_result = false;
    std::size_t waits = 0;
    fixture->on_wait = [&fixture, &waits] {
        ++waits;
        if (waits >= 3) {
            // Three bounded waits in, the peer "sends" EOF; the line already buffered is still
            // delivered, which is the EOF-terminated-frame rule.
            fixture->input_complete = true;
            fixture->wait_result = true;
        }
    };
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_call");
    expect(fixture->wait_calls >= 3, "mcp_call: the idle loop waited on the readiness seam");
    expect(responses.size() == 1, "mcp_call: and the buffered line was still answered");
    expect(fixture->last_wait.count() > 0, "mcp_call: the wait is bounded, not indefinite");
}

void test_stop_flag_is_observed() {
    // A silent client must not make the loop spin forever, and the stop flag must end it. The fake
    // stream reports "not ready" so the loop would idle on the bounded wait; the flag is already
    // raised, which is the state a signal handler leaves behind, and the loop must observe it
    // before it reads anything.
    Fixture fixture;
    fixture->input.clear();
    fixture->input_complete = false;
    fixture->wait_result = false;
    std::atomic<bool> stop{true};
    mcp::McpSession session;

    mcp::ServeOptions options;
    options.stop = &stop;
    const mcp::ServeResult result = mcp::serve_stdio(fixture.transport, session, options);
    expect(result.stopped, "mcp_call: the stop flag ends the loop");
    expect(fixture.stream.lines().empty(), "mcp_call: a stopped session wrote nothing");
}

void test_rejected_frame_is_answered() {
    // A framing rejection (here an over-long line) has no request to correlate with, so the loop
    // answers with a null-id parse error and the session stays usable for the next line.
    Fixture fixture;
    fixture->input = std::string(R"({"jsonrpc":"2.0","id":1,"method":"ping","pad":")") +
                     std::string(1 << 21, 'x') + "\"}\n" + line(kHandshake);
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_call");
    // The over-long line is dropped by the framing layer and answered with a parse error; the
    // handshake that follows is a normal frame and must still be handled.
    expect(responses.size() == 2, "mcp_call: the rejected frame and the next request are answered");
    if (responses.size() == 2) {
        expect(decode(responses[0], "mcp_call").find("error") != nullptr,
               "mcp_call: the rejected frame answered with an error");
        expect(decode(responses[1], "mcp_call").find("result") != nullptr,
               "mcp_call: the session stayed usable after a rejected frame");
    }
}

} // namespace

int main() {
    test_tools_call_round_trip();
    test_idle_waits_instead_of_spinning();
    test_stop_flag_is_observed();
    test_rejected_frame_is_answered();
    if (mcp_test::failures == 0) {
        std::cout << "mcp_call: ok\n";
    }
    return mcp_test::failures == 0 ? 0 : 1;
}
